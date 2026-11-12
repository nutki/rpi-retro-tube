#include "libretro.h"
#include "mainlog.h"
#include "maindispmanx.h"

#include <fcntl.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define MAX_ELEMENTS 16
#define MAX_ATOMIC_OBJECTS (MAX_ELEMENTS * 2 + 4)
#define MAX_ATOMIC_PROPERTIES (MAX_ELEMENTS * 16 + 16)
struct drm_buffer {
  uint32_t fb_id;
  uint32_t handle;
  uint32_t pitch;
  uint64_t size;
  void *map;
};

struct drm_plane {
  uint32_t id;
  uint32_t possible_crtcs;
  uint32_t current_crtc_id;
  uint32_t *formats;
  uint32_t format_count;
  uint32_t type;
  uint64_t zpos;
  uint64_t zpos_min;
  uint64_t zpos_max;
  uint32_t fb_id_prop;
  uint32_t crtc_id_prop;
  uint32_t src_x_prop;
  uint32_t src_y_prop;
  uint32_t src_w_prop;
  uint32_t src_h_prop;
  uint32_t crtc_x_prop;
  uint32_t crtc_y_prop;
  uint32_t crtc_w_prop;
  uint32_t crtc_h_prop;
  uint32_t zpos_prop;
  uint32_t scaling_filter_prop;
  uint32_t alpha_prop;
  uint64_t filter_default;
  uint64_t filter_nearest;
  int has_default_filter;
  int has_nearest_filter;
  int assigned;
};

struct frame_element {
  uint8_t *data;
  size_t data_size;
  int w, h, pitch, mode;
  float aspect;
  int x, y, zoom;
  int time_us;
  int used;
  int layer;
  int plane_index;
  uint32_t drm_format;
  int output_bpp;
  struct drm_buffer buffers[2];
  struct drm_buffer retired_buffers[2];
  int has_retired_buffers;
  int has_framebuffer;
  int displayed;
  int pending;
  int has_displayed;
  int frame_dirty;
  int surface_dirty;
  int plane_updated;
} frame_elements[MAX_ELEMENTS];

struct atomic_object {
  uint32_t id;
  uint32_t type;
  uint32_t property_count;
  uint32_t properties[16];
  uint64_t values[16];
};

struct atomic_request {
  struct atomic_object objects[MAX_ATOMIC_OBJECTS];
  uint32_t object_count;
  uint32_t property_count;
};

struct property_info {
  uint32_t id;
  uint32_t flags;
  uint64_t value;
  uint64_t min_value;
  uint64_t max_value;
  char name[DRM_PROP_NAME_LEN];
  struct drm_mode_property_enum *enums;
  uint32_t enum_count;
};

static int drm_fd = -1;
static uint32_t connector_id;
static uint32_t crtc_id;
static uint32_t crtc_index;
static drmModeModeInfo selected_mode;
static drmModeCrtcPtr original_crtc;
static int original_crtc_valid;
static uint32_t *original_connectors;
static uint32_t original_connector_count;
static uint32_t crtc_mode_id_prop;
static uint32_t crtc_active_prop;
static uint32_t connector_crtc_id_prop;
static uint32_t mode_blob_id;
static struct drm_plane drm_planes[MAX_ELEMENTS];
static int drm_plane_count;
static int display_ready;
static int kms_active;
static int filter_support_logged;
static int screenX, screenY;

static int drm_ioctl(unsigned long request, void *arg) {
  return drmIoctl(drm_fd, request, arg);
}

static int pixel_format_to_size(enum retro_pixel_format fmt) {
  switch (fmt) {
  case RETRO_PIXEL_FORMAT_XRGB8888:
    return 4;
  case RETRO_PIXEL_FORMAT_RGB565:
    return 2;
  case RETRO_PIXEL_FORMAT_UNKNOWN:
  case RETRO_PIXEL_FORMAT_0RGB1555:
    break;
  }
  rt_log("pixel format not supported: %d\n", fmt);
  return 0;
}

static uint32_t retro_format_to_drm(enum retro_pixel_format fmt) {
  switch (fmt) {
  case RETRO_PIXEL_FORMAT_XRGB8888:
    return DRM_FORMAT_XRGB8888;
  case RETRO_PIXEL_FORMAT_RGB565:
    return DRM_FORMAT_RGB565;
  default:
    return 0;
  }
}

void dispmanx_set_pos(struct frame_element *fe, int dx, int dy, int zoom) {
  if (!fe)
    return;
  fe->x = dx;
  fe->y = dy;
  fe->zoom = zoom;
}

static int apply_zoom(const struct frame_element *fe, int value) {
  int zoom = fe->zoom;
  if (fe->h > (fe->time_us < 19000 ? 240 : 288))
    zoom /= 2;
  return (int)((int64_t)value * zoom >> 8);
}

static int get_property_info(uint32_t property_id,
                             struct property_info *info) {
  drmModePropertyPtr property = drmModeGetProperty(drm_fd, property_id);
  if (!property)
    return 0;

  info->id = property_id;
  info->flags = property->flags;
  memcpy(info->name, property->name, sizeof(info->name));
  info->enum_count = property->count_enums;
  if (property->flags & DRM_MODE_PROP_RANGE) {
    if (property->count_values < 2) {
      drmModeFreeProperty(property);
      return 0;
    }
    info->min_value = property->values[0];
    info->max_value = property->values[1];
  }
  if ((property->flags & DRM_MODE_PROP_ENUM) && info->enum_count) {
    info->enums = calloc(info->enum_count, sizeof(*info->enums));
    if (!info->enums) {
      perror("allocating DRM property enums");
      drmModeFreeProperty(property);
      return 0;
    }
    memcpy(info->enums, property->enums,
           info->enum_count * sizeof(*info->enums));
  }
  drmModeFreeProperty(property);
  return 1;
}

static void free_property_info(struct property_info *info) {
  free(info->enums);
  info->enums = NULL;
}

static int get_object_properties(uint32_t object_id, uint32_t object_type,
                                 uint32_t **ids, uint64_t **values,
                                 uint32_t *count) {
  drmModeObjectPropertiesPtr properties =
      drmModeObjectGetProperties(drm_fd, object_id, object_type);
  if (!properties)
    return 0;

  *count = properties->count_props;
  *ids = calloc(*count, sizeof(**ids));
  *values = calloc(*count, sizeof(**values));
  if ((!*ids || !*values) && *count) {
    free(*ids);
    free(*values);
    *ids = NULL;
    *values = NULL;
    perror("allocating DRM object properties");
    drmModeFreeObjectProperties(properties);
    return 0;
  }
  if (*count) {
    memcpy(*ids, properties->props, *count * sizeof(**ids));
    memcpy(*values, properties->prop_values, *count * sizeof(**values));
  }
  drmModeFreeObjectProperties(properties);
  return 1;
}

static uint32_t find_object_property(uint32_t object_id, uint32_t object_type,
                                     const char *name, uint64_t *value) {
  uint32_t *ids = NULL, count = 0, result = 0;
  uint64_t *values = NULL;
  if (!get_object_properties(object_id, object_type, &ids, &values, &count))
    return 0;

  for (uint32_t i = 0; i < count; i++) {
    struct property_info info = {0};
    if (!get_property_info(ids[i], &info))
      continue;
    if (!strcmp(info.name, name)) {
      result = ids[i];
      if (value)
        *value = values[i];
      free_property_info(&info);
      break;
    }
    free_property_info(&info);
  }
  free(ids);
  free(values);
  return result;
}

static uint64_t enum_value(const struct property_info *info, const char *name,
                           int *found) {
  for (uint32_t i = 0; i < info->enum_count; i++) {
    if (!strcasecmp(info->enums[i].name, name)) {
      *found = 1;
      return info->enums[i].value;
    }
  }
  *found = 0;
  return 0;
}

static int capture_original_connectors(const drmModeResPtr resources,
                                       uint32_t target_crtc_id) {
  original_connectors =
      calloc((size_t)resources->count_connectors, sizeof(*original_connectors));
  if (!original_connectors && resources->count_connectors) {
    perror("allocating previous DRM connectors");
    return 0;
  }
  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnectorPtr connector =
        drmModeGetConnector(drm_fd, resources->connectors[i]);
    if (!connector)
      continue;
    if (connector->encoder_id) {
      drmModeEncoderPtr encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
      if (encoder) {
        if (encoder->crtc_id == target_crtc_id)
          original_connectors[original_connector_count++] =
              resources->connectors[i];
        drmModeFreeEncoder(encoder);
      }
    }
    drmModeFreeConnector(connector);
  }
  return 1;
}

static int get_connector_and_mode(drmModeModeInfoPtr mode,
                                  uint32_t *selected_connector,
                                  uint32_t *selected_crtc,
                                  uint32_t *selected_crtc_index) {
  drmModeResPtr resources = drmModeGetResources(drm_fd);
  if (!resources) {
    perror("drmModeGetResources");
    return 0;
  }
  int found = 0;

  int64_t best_score = -1;
  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnectorPtr connector =
        drmModeGetConnector(drm_fd, resources->connectors[i]);
    if (!connector)
      continue;
    if (connector->connection != DRM_MODE_CONNECTED ||
        !connector->count_modes) {
      drmModeFreeConnector(connector);
      continue;
    }

    uint32_t candidate_crtc = 0, candidate_index = 0;
    for (int e = 0; e < connector->count_encoders && !candidate_crtc; e++) {
      drmModeEncoderPtr encoder =
          drmModeGetEncoder(drm_fd, connector->encoders[e]);
      if (!encoder)
        continue;
      if (encoder->crtc_id) {
        for (int c = 0; c < resources->count_crtcs; c++)
          if (resources->crtcs[c] == encoder->crtc_id) {
            candidate_crtc = encoder->crtc_id;
            candidate_index = c;
          }
      }
      if (!candidate_crtc) {
        for (int c = 0; c < resources->count_crtcs; c++)
          if (encoder->possible_crtcs & (1u << c)) {
            candidate_crtc = resources->crtcs[c];
            candidate_index = c;
            break;
          }
      }
      drmModeFreeEncoder(encoder);
    }

    if (candidate_crtc) {
      for (int m = 0; m < connector->count_modes; m++) {
        int64_t score =
            (int64_t)connector->modes[m].hdisplay * connector->modes[m].vdisplay;
        if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
          score += INT64_C(1) << 40;
        if (score > best_score) {
          best_score = score;
          *mode = connector->modes[m];
          *selected_connector = resources->connectors[i];
          *selected_crtc = candidate_crtc;
          *selected_crtc_index = candidate_index;
          found = 1;
        }
      }
    }
    drmModeFreeConnector(connector);
  }

  if (found && !capture_original_connectors(resources, *selected_crtc))
    found = 0;

  drmModeFreeResources(resources);
  if (!found)
    rt_log("no connected KMS connector with a usable mode was found\n");
  return found;
}

static int plane_has_format(const struct drm_plane *plane, uint32_t format) {
  for (uint32_t i = 0; i < plane->format_count; i++)
    if (plane->formats[i] == format)
      return 1;
  return 0;
}

static int plane_compare(const void *left, const void *right) {
  const struct drm_plane *a = left;
  const struct drm_plane *b = right;
  if (a->type != b->type)
    return a->type < b->type ? -1 : 1;
  if (a->zpos != b->zpos)
    return a->zpos < b->zpos ? -1 : 1;
  return a->id < b->id ? -1 : a->id > b->id;
}

static int add_plane_property(struct drm_plane *plane, uint32_t property_id,
                              uint64_t current_value,
                              const char *expected_name, uint32_t *storage) {
  struct property_info info = {0};
  if (!get_property_info(property_id, &info))
    return 0;

  int found = 0;
  if (!strcmp(expected_name, "type")) {
    if (!strcasecmp(info.name, "type")) {
      if (!info.enums || !info.enum_count) {
        free_property_info(&info);
        return 0;
      }
      for (uint32_t i = 0; i < info.enum_count; i++) {
        if (info.enums[i].value != current_value)
          continue;
        if (!strcasecmp(info.enums[i].name, "Primary"))
          plane->type = DRM_PLANE_TYPE_PRIMARY;
        else if (!strcasecmp(info.enums[i].name, "Overlay"))
          plane->type = DRM_PLANE_TYPE_OVERLAY;
        else if (!strcasecmp(info.enums[i].name, "Cursor"))
          plane->type = DRM_PLANE_TYPE_CURSOR;
        found = 1;
        break;
      }
    }
  } else if (!strcmp(expected_name, "SCALING_FILTER")) {
    if (!strcmp(info.name, expected_name) && info.enums) {
      plane->scaling_filter_prop = property_id;
      plane->filter_default =
          enum_value(&info, "Default", &plane->has_default_filter);
      plane->filter_nearest =
          enum_value(&info, "Nearest Neighbor", &plane->has_nearest_filter);
      found = 1;
    }
  } else if (!strcmp(expected_name, "zpos")) {
    if (!strcmp(info.name, expected_name)) {
      plane->zpos = current_value;
      plane->zpos_min = info.min_value;
      plane->zpos_max = info.max_value;
      if (!(info.flags & DRM_MODE_PROP_IMMUTABLE))
        plane->zpos_prop = property_id;
      found = 1;
    }
  } else if (!strcmp(expected_name, "alpha")) {
    if (!strcmp(info.name, expected_name)) {
      if (!(info.flags & DRM_MODE_PROP_IMMUTABLE))
        plane->alpha_prop = property_id;
      found = 1;
    }
  } else if (!strcmp(info.name, expected_name)) {
    *storage = property_id;
    found = 1;
  }
  free_property_info(&info);
  return found;
}

static int populate_plane_properties(struct drm_plane *plane) {
  uint32_t *ids = NULL, count = 0;
  uint64_t *values = NULL;
  if (!get_object_properties(plane->id, DRM_MODE_OBJECT_PLANE, &ids, &values,
                             &count))
    return 0;

  int have_type = 0;
  for (uint32_t i = 0; i < count; i++) {
    struct property_info info = {0};
    if (!get_property_info(ids[i], &info))
      continue;
    if (!strcmp(info.name, "type")) {
      free_property_info(&info);
      have_type = add_plane_property(plane, ids[i], values[i], "type", NULL);
      continue;
    }
    if (!strcmp(info.name, "SCALING_FILTER")) {
      free_property_info(&info);
      add_plane_property(plane, ids[i], values[i], "SCALING_FILTER", NULL);
      continue;
    }
    if (!strcmp(info.name, "zpos")) {
      free_property_info(&info);
      add_plane_property(plane, ids[i], values[i], "zpos", NULL);
      continue;
    }
    if (!strcmp(info.name, "alpha")) {
      free_property_info(&info);
      add_plane_property(plane, ids[i], values[i], "alpha", NULL);
      continue;
    }
    uint32_t *destination = NULL;
    if (!strcmp(info.name, "FB_ID"))
      destination = &plane->fb_id_prop;
    else if (!strcmp(info.name, "CRTC_ID"))
      destination = &plane->crtc_id_prop;
    else if (!strcmp(info.name, "SRC_X"))
      destination = &plane->src_x_prop;
    else if (!strcmp(info.name, "SRC_Y"))
      destination = &plane->src_y_prop;
    else if (!strcmp(info.name, "SRC_W"))
      destination = &plane->src_w_prop;
    else if (!strcmp(info.name, "SRC_H"))
      destination = &plane->src_h_prop;
    else if (!strcmp(info.name, "CRTC_X"))
      destination = &plane->crtc_x_prop;
    else if (!strcmp(info.name, "CRTC_Y"))
      destination = &plane->crtc_y_prop;
    else if (!strcmp(info.name, "CRTC_W"))
      destination = &plane->crtc_w_prop;
    else if (!strcmp(info.name, "CRTC_H"))
      destination = &plane->crtc_h_prop;
    if (destination)
      *destination = ids[i];
    free_property_info(&info);
  }
  free(ids);
  free(values);

  return have_type && plane->fb_id_prop && plane->crtc_id_prop &&
         plane->src_x_prop && plane->src_y_prop && plane->src_w_prop &&
         plane->src_h_prop && plane->crtc_x_prop && plane->crtc_y_prop &&
         plane->crtc_w_prop && plane->crtc_h_prop;
}

static int enumerate_planes(void) {
  drmModePlaneResPtr resources = drmModeGetPlaneResources(drm_fd);
  if (!resources) {
    perror("drmModeGetPlaneResources");
    return 0;
  }
  if (!resources->count_planes) {
    drmModeFreePlaneResources(resources);
    return 0;
  }

  for (uint32_t i = 0;
       i < resources->count_planes && drm_plane_count < MAX_ELEMENTS;
       i++) {
    drmModePlanePtr query = drmModeGetPlane(drm_fd, resources->planes[i]);
    if (!query)
      continue;
    if (!(query->possible_crtcs & (1u << crtc_index))) {
      drmModeFreePlane(query);
      continue;
    }

    struct drm_plane plane = {
        .id = query->plane_id,
        .possible_crtcs = query->possible_crtcs,
        .current_crtc_id = query->crtc_id,
        .formats = query->formats,
        .format_count = query->count_formats,
    };
    query->formats = NULL;
    drmModeFreePlane(query);
    if (plane.current_crtc_id && plane.current_crtc_id != crtc_id) {
      free(plane.formats);
      continue;
    }
    if (!populate_plane_properties(&plane) ||
        plane.type != DRM_PLANE_TYPE_OVERLAY ||
        (!plane_has_format(&plane, DRM_FORMAT_XRGB8888) &&
         !plane_has_format(&plane, DRM_FORMAT_RGB565))) {
      free(plane.formats);
      continue;
    }
    drm_planes[drm_plane_count++] = plane;
  }
  drmModeFreePlaneResources(resources);
  qsort(drm_planes, drm_plane_count, sizeof(drm_planes[0]), plane_compare);
  if (!drm_plane_count)
    rt_log("no usable DRM planes support this CRTC and RGB frame formats\n");
  else
    rt_log("found %d usable DRM planes for CRTC %u\n", drm_plane_count,
           crtc_id);
  return drm_plane_count > 0;
}

static int add_atomic_property(struct atomic_request *request, uint32_t object_id,
                               uint32_t object_type, uint32_t property_id,
                               uint64_t value) {
  uint32_t object_index = 0;
  while (object_index < request->object_count &&
         (request->objects[object_index].id != object_id ||
          request->objects[object_index].type != object_type))
    object_index++;

  if (object_index == request->object_count) {
    if (request->object_count == MAX_ATOMIC_OBJECTS)
      return 0;
    request->objects[object_index].id = object_id;
    request->objects[object_index].type = object_type;
    request->object_count++;
  }

  struct atomic_object *object = &request->objects[object_index];
  for (uint32_t i = 0; i < object->property_count; i++) {
    if (object->properties[i] == property_id) {
      object->values[i] = value;
      return 1;
    }
  }
  if (object->property_count == 16 ||
      request->property_count == MAX_ATOMIC_PROPERTIES)
    return 0;
  object->properties[object->property_count] = property_id;
  object->values[object->property_count] = value;
  object->property_count++;
  request->property_count++;
  return 1;
}

static int commit_atomic(struct atomic_request *request, uint32_t flags) {
  drmModeAtomicReqPtr atomic = drmModeAtomicAlloc();
  if (!atomic) {
    perror("drmModeAtomicAlloc");
    return -1;
  }
  for (uint32_t i = 0; i < request->object_count; i++) {
    const struct atomic_object *object = &request->objects[i];
    for (uint32_t p = 0; p < object->property_count; p++) {
      if (drmModeAtomicAddProperty(atomic, object->id, object->properties[p],
                                   object->values[p]) < 0) {
        drmModeAtomicFree(atomic);
        return -1;
      }
    }
  }
  int result = drmModeAtomicCommit(drm_fd, atomic, flags, NULL);
  drmModeAtomicFree(atomic);
  return result;
}

static int add_plane_state(struct atomic_request *request,
                           struct frame_element *fe, uint32_t fb_id,
                           int enable, uint64_t filter_value,
                           uint64_t zpos_value) {
  struct drm_plane *plane = &drm_planes[fe->plane_index];
  if (!add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->fb_id_prop, enable ? fb_id : 0) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->crtc_id_prop, enable ? crtc_id : 0))
    return 0;
  if (!enable) {
    return add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->src_x_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->src_y_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->src_w_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->src_h_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->crtc_x_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->crtc_y_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->crtc_w_prop, 0) &&
           add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                               plane->crtc_h_prop, 0);
  }

  int target_w = (int)(((int64_t)fe->w * screenX * 3 / 4 / screenY) *
                       (fe->aspect * fe->h / fe->w));
  int target_h = fe->h;
  int dest_w = apply_zoom(fe, target_w);
  int dest_h = apply_zoom(fe, target_h);
  if (dest_w <= 0 || dest_h <= 0)
    return 0;
  int dest_x = (screenX - dest_w) / 2 + fe->x;
  int dest_y = (screenY - dest_h) / 2 + fe->y;

  if (!add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->src_x_prop, 0) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->src_y_prop, 0) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->src_w_prop, (uint64_t)fe->w << 16) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->src_h_prop, (uint64_t)fe->h << 16) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->crtc_x_prop, (uint64_t)(int64_t)dest_x) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->crtc_y_prop, (uint64_t)(int64_t)dest_y) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->crtc_w_prop, (uint64_t)dest_w) ||
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->crtc_h_prop, (uint64_t)dest_h))
    return 0;
  if (plane->scaling_filter_prop && plane->has_default_filter &&
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->scaling_filter_prop, filter_value))
    return 0;
  if (plane->zpos_prop &&
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->zpos_prop, zpos_value))
    return 0;
  if (plane->alpha_prop &&
      !add_atomic_property(request, plane->id, DRM_MODE_OBJECT_PLANE,
                           plane->alpha_prop, 0xFFFF))
    return 0;
  return 1;
}

static int create_mode_blob(void) {
  if (drmModeCreatePropertyBlob(drm_fd, &selected_mode, sizeof(selected_mode),
                                &mode_blob_id) < 0) {
    perror("drmModeCreatePropertyBlob");
    return 0;
  }
  return 1;
}

static int make_buffer(uint32_t width, uint32_t height, int bpp,
                       uint32_t drm_format, struct drm_buffer *buffer) {
  struct drm_mode_create_dumb create = {
      .width = width,
      .height = height,
      .bpp = (uint32_t)bpp,
  };
  if (drm_ioctl(DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    perror("DRM_IOCTL_MODE_CREATE_DUMB");
    return 0;
  }
  buffer->handle = create.handle;
  buffer->pitch = create.pitch;
  buffer->size = create.size;

  uint32_t handles[4] = {create.handle};
  uint32_t pitches[4] = {create.pitch};
  uint32_t offsets[4] = {0};
  if (drmModeAddFB2(drm_fd, width, height, drm_format, handles, pitches,
                    offsets, &buffer->fb_id, 0) < 0) {
    perror("drmModeAddFB2");
    return 0;
  }

  struct drm_mode_map_dumb map = {.handle = create.handle};
  if (drm_ioctl(DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
    perror("DRM_IOCTL_MODE_MAP_DUMB");
    return 0;
  }
  buffer->map = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, drm_fd, (off_t)map.offset);
  if (buffer->map == MAP_FAILED) {
    buffer->map = NULL;
    perror("mmap DRM plane buffer");
    return 0;
  }
  return 1;
}

static void destroy_buffer(struct drm_buffer *buffer) {
  if (buffer->map) {
    munmap(buffer->map, (size_t)buffer->size);
    buffer->map = NULL;
  }
  if (buffer->fb_id) {
    if (drmModeRmFB(drm_fd, buffer->fb_id) < 0)
      perror("drmModeRmFB");
    buffer->fb_id = 0;
  }
  if (buffer->handle) {
    struct drm_mode_destroy_dumb destroy = {.handle = buffer->handle};
    if (drm_ioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
      perror("DRM_IOCTL_MODE_DESTROY_DUMB");
    buffer->handle = 0;
  }
}

static void destroy_element_buffers(struct drm_buffer buffers[2]) {
  destroy_buffer(&buffers[0]);
  destroy_buffer(&buffers[1]);
}

static int element_output_format(struct frame_element *fe,
                                 enum retro_pixel_format input_format) {
  struct drm_plane *plane = &drm_planes[fe->plane_index];
  uint32_t requested = retro_format_to_drm(input_format);
  if (requested && plane_has_format(plane, requested)) {
    fe->drm_format = requested;
    fe->output_bpp = input_format == RETRO_PIXEL_FORMAT_RGB565 ? 16 : 32;
    return 1;
  }
  if (plane_has_format(plane, DRM_FORMAT_XRGB8888)) {
    fe->drm_format = DRM_FORMAT_XRGB8888;
    fe->output_bpp = 32;
    return 1;
  }
  if (plane_has_format(plane, DRM_FORMAT_RGB565)) {
    fe->drm_format = DRM_FORMAT_RGB565;
    fe->output_bpp = 16;
    return 1;
  }
  return 0;
}

static int configure_element_buffers(struct frame_element *fe) {
  struct drm_buffer new_buffers[2] = {{0}};
  if (!make_buffer((uint32_t)fe->w, (uint32_t)fe->h, fe->output_bpp,
                   fe->drm_format, &new_buffers[0]) ||
      !make_buffer((uint32_t)fe->w, (uint32_t)fe->h, fe->output_bpp,
                   fe->drm_format, &new_buffers[1])) {
    destroy_element_buffers(new_buffers);
    return 0;
  }

  if (fe->has_retired_buffers)
    destroy_element_buffers(fe->buffers);
  else if (fe->has_framebuffer) {
    memcpy(fe->retired_buffers, fe->buffers, sizeof(fe->buffers));
    memset(fe->buffers, 0, sizeof(fe->buffers));
    fe->has_retired_buffers = 1;
  }
  memcpy(fe->buffers, new_buffers, sizeof(new_buffers));
  fe->has_framebuffer = 1;
  fe->surface_dirty = 0;
  fe->has_displayed = 0;
  fe->displayed = 1;
  fe->frame_dirty = 1;
  return 1;
}

static uint32_t source_pixel(const struct frame_element *fe, int x, int y) {
  const uint8_t *pixel =
      fe->data + (size_t)y * fe->pitch + (size_t)x * pixel_format_to_size(fe->mode);
  if (fe->mode == RETRO_PIXEL_FORMAT_XRGB8888) {
    uint32_t value;
    memcpy(&value, pixel, sizeof(value));
    return value & 0x00ffffff;
  }
  uint16_t value;
  memcpy(&value, pixel, sizeof(value));
  uint32_t red = ((value >> 11) & 0x1f) * 255 / 31;
  uint32_t green = ((value >> 5) & 0x3f) * 255 / 63;
  uint32_t blue = (value & 0x1f) * 255 / 31;
  return (red << 16) | (green << 8) | blue;
}

static void copy_to_drm_buffer(struct frame_element *fe,
                               struct drm_buffer *buffer) {
  uint8_t *destination = buffer->map;
  int source_pixel_size = pixel_format_to_size(fe->mode);
  int destination_pixel_size = fe->output_bpp / 8;
  int copy_width = fe->w * destination_pixel_size;
  if (fe->mode == RETRO_PIXEL_FORMAT_XRGB8888 &&
      fe->drm_format == DRM_FORMAT_XRGB8888)
    copy_width = fe->w * 4;
  else if (fe->mode == RETRO_PIXEL_FORMAT_RGB565 &&
           fe->drm_format == DRM_FORMAT_RGB565)
    copy_width = fe->w * 2;

  for (int y = 0; y < fe->h; y++) {
    uint8_t *dest_row = destination + (size_t)y * buffer->pitch;
    const uint8_t *source_row = fe->data + (size_t)y * fe->pitch;
    if ((source_pixel_size == destination_pixel_size) &&
        ((source_pixel_size == 4 && fe->drm_format == DRM_FORMAT_XRGB8888) ||
         (source_pixel_size == 2 && fe->drm_format == DRM_FORMAT_RGB565))) {
      memcpy(dest_row, source_row, (size_t)copy_width);
      continue;
    }
    for (int x = 0; x < fe->w; x++) {
      uint32_t pixel = source_pixel(fe, x, y);
      if (fe->drm_format == DRM_FORMAT_XRGB8888) {
        memcpy(dest_row + (size_t)x * 4, &pixel, sizeof(pixel));
      } else {
        uint16_t rgb565 =
            (uint16_t)(((pixel >> 8) & 0xf800) | ((pixel >> 5) & 0x07e0) |
                       ((pixel >> 3) & 0x001f));
        memcpy(dest_row + (size_t)x * 2, &rgb565, sizeof(rgb565));
      }
    }
  }
}

static int atomic_add_or_report(struct atomic_request *request, uint32_t object_id,
                                uint32_t object_type, uint32_t property_id,
                                uint64_t value) {
  if (add_atomic_property(request, object_id, object_type, property_id, value))
    return 1;
  rt_log("too many properties in DRM atomic commit\n");
  return 0;
}

static int add_initial_plane_disables(struct atomic_request *request) {
  for (int p = 0; p < drm_plane_count; p++) {
    struct drm_plane *plane = &drm_planes[p];
    if (plane->current_crtc_id != crtc_id)
      continue;
    if (!atomic_add_or_report(request, plane->id, DRM_MODE_OBJECT_PLANE,
                              plane->fb_id_prop, 0) ||
        !atomic_add_or_report(request, plane->id, DRM_MODE_OBJECT_PLANE,
                              plane->crtc_id_prop, 0))
      return 0;
  }
  return 1;
}

static int commit_frame(void) {
  struct atomic_request request = {0};
  int modeset = !kms_active;
  int fullscreen = 0;

  for (int i = 0; i < MAX_ELEMENTS; i++) {
    struct frame_element *fe = &frame_elements[i];
    fe->plane_updated = 0;
    if (fe->used == 1 && fe->plane_index >= 0 && fe->data)
      fullscreen |= fe->x == 0 && fe->y == 0 && fe->zoom == 0x200;
  }
  uint64_t filter_value = fullscreen ? 1 : 0;

  if (modeset) {
    if (!atomic_add_or_report(&request, crtc_id, DRM_MODE_OBJECT_CRTC,
                              crtc_mode_id_prop, mode_blob_id) ||
        !atomic_add_or_report(&request, crtc_id, DRM_MODE_OBJECT_CRTC,
                              crtc_active_prop, 1) ||
        !atomic_add_or_report(&request, connector_id,
                              DRM_MODE_OBJECT_CONNECTOR,
                              connector_crtc_id_prop, crtc_id) ||
        !add_initial_plane_disables(&request))
      return 0;
    for (uint32_t i = 0; i < original_connector_count; i++) {
      if (original_connectors[i] == connector_id)
        continue;
      uint32_t prop =
          find_object_property(original_connectors[i],
                               DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
      if (prop && !atomic_add_or_report(&request, original_connectors[i],
                                        DRM_MODE_OBJECT_CONNECTOR, prop, 0))
        return 0;
    }
  }

  for (int i = 0; i < MAX_ELEMENTS; i++) {
    struct frame_element *fe = &frame_elements[i];
    if (fe->used == 2 && fe->plane_index >= 0) {
      if (!add_plane_state(&request, fe, 0, 0, 0, 0))
        return 0;
      continue;
    }
    if (fe->used != 1 || fe->plane_index < 0 || !fe->data)
      continue;

    if (!fe->has_framebuffer || fe->surface_dirty) {
      if (!element_output_format(fe, fe->mode) ||
          !configure_element_buffers(fe)) {
        rt_log("could not create framebuffers for DRM plane %u\n",
               drm_planes[fe->plane_index].id);
        continue;
      }
    }
    int buffer_index = fe->has_displayed ? 1 - fe->displayed : 0;
    if (fe->frame_dirty)
      copy_to_drm_buffer(fe, &fe->buffers[buffer_index]);
    else
      buffer_index = fe->displayed;
    fe->pending = buffer_index;

    struct drm_plane *plane = &drm_planes[fe->plane_index];
    uint64_t plane_filter = filter_value;
    if (!plane->has_default_filter ||
        (fullscreen && !plane->has_nearest_filter)) {
      if (!filter_support_logged) {
        rt_log("one or more DRM planes do not expose both SCALING_FILTER modes\n");
        filter_support_logged = 1;
      }
      plane_filter = plane->filter_default;
    } else {
      plane_filter = fullscreen ? plane->filter_nearest : plane->filter_default;
    }
    if (!add_plane_state(&request, fe, fe->buffers[buffer_index].fb_id, 1,
                         plane_filter, plane->zpos))
      return 0;
    fe->plane_updated = 1;
  }

  if (!request.object_count)
    return 1;
  if (commit_atomic(&request, modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0) < 0) {
    perror("drmModeAtomicCommit");
    return 0;
  }
  if (modeset)
    kms_active = 1;

  for (int i = 0; i < MAX_ELEMENTS; i++) {
    struct frame_element *fe = &frame_elements[i];
    if (fe->used == 2) {
      free(fe->data);
      destroy_element_buffers(fe->buffers);
      destroy_element_buffers(fe->retired_buffers);
      if (fe->plane_index >= 0)
        drm_planes[fe->plane_index].assigned = 0;
      memset(fe, 0, sizeof(*fe));
      fe->plane_index = -1;
    } else if (fe->used == 1 && fe->data && fe->has_framebuffer &&
               fe->plane_updated) {
      fe->has_displayed = 1;
      fe->displayed = fe->pending;
      fe->frame_dirty = 0;
      fe->plane_updated = 0;
      if (fe->has_retired_buffers) {
        destroy_element_buffers(fe->retired_buffers);
        fe->has_retired_buffers = 0;
      }
    }
  }
  return 1;
}

void dispmanx_show(void) {
  if (!display_ready)
    return;
  commit_frame();
}

void dispmanx_update_frame(struct frame_element *fe, struct video_frame *frame,
                           void *frame_data) {
  if (!fe || !frame || !frame_data)
    return;
  int pixel_size = pixel_format_to_size(frame->fmt);
  if (!pixel_size)
    return;
  if (frame->w <= 0 || frame->h <= 0 || frame->pitch <= 0 ||
      frame->pitch % pixel_size || frame->pitch / pixel_size < frame->w ||
      (size_t)frame->pitch > SIZE_MAX / (size_t)frame->h) {
    rt_log("invalid video frame dimensions or pitch: %dx%d pitch %d\n",
           frame->w, frame->h, frame->pitch);
    return;
  }
  size_t data_size = (size_t)frame->pitch * (size_t)frame->h;
  if (fe->data_size != data_size) {
    uint8_t *data = realloc(fe->data, data_size);
    if (!data) {
      perror("allocating video frame data");
      return;
    }
    fe->data = data;
    fe->data_size = data_size;
  }
  memcpy(fe->data, frame_data, data_size);

  int dimensions_changed =
      !fe->has_framebuffer || fe->w != frame->w || fe->h != frame->h ||
      fe->mode != frame->fmt;
  fe->w = frame->w;
  fe->h = frame->h;
  fe->pitch = frame->pitch;
  fe->mode = frame->fmt;
  fe->aspect = frame->aspect
                   ? frame->aspect
                   : (frame->h ? (float)frame->w / frame->h : 1);
  fe->time_us = frame->time_us;
  if (dimensions_changed)
    fe->surface_dirty = 1;
  fe->frame_dirty = 1;
}

void dispmanx_init(void) {
  for (int card = 0; card < 16; card++) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/dev/dri/card%d", card);
    drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
      continue;
    drmVersionPtr version = drmGetVersion(drm_fd);
    if (!version || strcmp(version->name, "vc4")) {
      if (version)
        drmFreeVersion(version);
      close(drm_fd);
      drm_fd = -1;
      continue;
    }
    drmFreeVersion(version);
    if (drmSetMaster(drm_fd) < 0) {
      perror("drmSetMaster on VC4");
      close(drm_fd);
      drm_fd = -1;
      continue;
    }
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
      perror("enabling VC4 atomic modesetting");
      drmDropMaster(drm_fd);
      close(drm_fd);
      drm_fd = -1;
      continue;
    }
    break;
  }
  if (drm_fd < 0) {
    rt_log("could not open a VC4 DRM card with atomic modesetting\n");
    return;
  }

  if (!get_connector_and_mode(&selected_mode, &connector_id, &crtc_id,
                              &crtc_index)) {
    dispmanx_close();
    return;
  }
  original_crtc = drmModeGetCrtc(drm_fd, crtc_id);
  if (!original_crtc) {
    perror("drmModeGetCrtc");
    dispmanx_close();
    return;
  }
  original_crtc_valid = 1;

  crtc_mode_id_prop =
      find_object_property(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
  crtc_active_prop =
      find_object_property(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
  connector_crtc_id_prop = find_object_property(
      connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
  if (!crtc_mode_id_prop || !crtc_active_prop || !connector_crtc_id_prop) {
    rt_log("KMS atomic modesetting properties are incomplete\n");
    dispmanx_close();
    return;
  }
  if (!enumerate_planes() || !create_mode_blob()) {
    dispmanx_close();
    return;
  }

  screenX = selected_mode.hdisplay;
  screenY = selected_mode.vdisplay;
  if (screenX <= 0 || screenY <= 0) {
    rt_log("selected DRM mode has invalid dimensions\n");
    dispmanx_close();
    return;
  }
  display_ready = 1;
  rt_log("KMS initialized: %ux%u at %u Hz\n", selected_mode.hdisplay,
         selected_mode.vdisplay, selected_mode.vrefresh);
}

static void restore_crtc(void) {
  if (!original_crtc_valid)
    return;
  drmModeModeInfoPtr mode =
      original_crtc->mode_valid ? &original_crtc->mode : NULL;
  if (drmModeSetCrtc(drm_fd, crtc_id, original_crtc->buffer_id,
                     original_crtc->x, original_crtc->y, original_connectors,
                     original_crtc->mode_valid ? original_connector_count : 0,
                     mode) < 0)
    perror("restoring DRM CRTC with drmModeSetCrtc");
  drmModeFreeCrtc(original_crtc);
  original_crtc = NULL;
  original_crtc_valid = 0;
}

void dispmanx_close(void) {
  if (drm_fd < 0)
    return;
  display_ready = 0;

  if (kms_active)
    restore_crtc();
  for (int i = 0; i < MAX_ELEMENTS; i++) {
    free(frame_elements[i].data);
    destroy_element_buffers(frame_elements[i].buffers);
    destroy_element_buffers(frame_elements[i].retired_buffers);
    memset(&frame_elements[i], 0, sizeof(frame_elements[i]));
    frame_elements[i].plane_index = -1;
  }
  for (int i = 0; i < drm_plane_count; i++) {
    free(drm_planes[i].formats);
    memset(&drm_planes[i], 0, sizeof(drm_planes[i]));
  }
  drm_plane_count = 0;
  if (mode_blob_id) {
    if (drmModeDestroyPropertyBlob(drm_fd, mode_blob_id) < 0)
      perror("drmModeDestroyPropertyBlob");
    mode_blob_id = 0;
  }
  free(original_connectors);
  original_connectors = NULL;
  original_connector_count = 0;
  if (drmDropMaster(drm_fd) < 0)
    perror("drmDropMaster");
  close(drm_fd);
  drm_fd = -1;
  kms_active = 0;
  original_crtc_valid = 0;
  display_ready = 0;
  filter_support_logged = 0;
  crtc_mode_id_prop = 0;
  crtc_active_prop = 0;
  connector_crtc_id_prop = 0;
  connector_id = 0;
  crtc_id = 0;
  crtc_index = 0;
}
//  010297 ALLOC 0 layer 9 id 107
//  010470 udev input initialized
// <gb> </home/pi/RetroPie/roms/gb/Super Mario Land (W) (V1.0) [!].gb>
// <gb> </home/pi/RetroPie/roms/gb/Dr. Mario (W) (V1.1).gb>
// <gb> </home/pi/RetroPie/roms/gb/Tetris (W) (V1.1) [!].gb>
//  010800 ALLOC 1 layer 10 id 118
//  010999 ALLOC 2 layer 10 id 129
//  011246 ALLOC 3 layer 10 id 140

struct frame_element *dispmanx_new_element(int layer) {
  int free_element = -1;
  int free_plane = -1;
  for (int i = 0; i < MAX_ELEMENTS; i++)
    if (!frame_elements[i].used) {
      free_element = i;
      break;
    }
  for (int i = 0; i < drm_plane_count; i++)
    if (!drm_planes[i].assigned) {
      free_plane = i;
      break;
    }
  if (free_element < 0 || free_plane < 0) {
    rt_log("no free DRM plane for layer %d (available planes: %d)\n", layer,
           drm_plane_count);
    return NULL;
  }

  struct frame_element *fe = &frame_elements[free_element];
  memset(fe, 0, sizeof(*fe));
  fe->used = 1;
  fe->layer = layer;
  fe->plane_index = free_plane;
  drm_planes[free_plane].assigned = 1;
  rt_log("ALLOC %d layer %d id %d\n", free_plane, layer, drm_planes[free_plane].id);
  return fe;
}

void dispmanx_free_element(struct frame_element *fe) {
  if (fe)
    fe->used = 2;
}
