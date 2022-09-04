
#include "mainlog.h"
#include <bcm_host.h>

int last_mode = -1;
extern int sdtv_aspect;
void sdtv_change_mode(int is_50hz, int is_interlaced) {
  SDTV_MODE_T mode = is_50hz ? SDTV_MODE_PAL : SDTV_MODE_NTSC;
  if (!is_interlaced)
    mode |= SDTV_MODE_PROGRESSIVE;
  SDTV_OPTIONS_T options = {aspect : sdtv_aspect};
  if (last_mode == mode)
    return;
  last_mode = mode;
  vc_tv_sdtv_power_on_id(0, mode, &options);
}

#define FILTER_ON_CMD                                                                                                  \
  "scaling_kernel "                                                                                                    \
  "0 -2 -6 -8 -10 -8 -3 2 18 50 82 119 155 187 213 227 227 213 187 155 119 82 50 18 2 -3 -8 -10 -8 -6 -2 0 0"
#define FILTER_OFF_CMD                                                                                                 \
  "scaling_kernel "                                                                                                    \
  "0 0 0 0 0 0 0 0 1 1 1 1 255 255 255 255 255 255 255 255 1 1 1 1 0 0 0 0 0 0 0 0 1"

/*
As seen in: https://github.com/raspberrypi/userland/blob/master/host_applications/linux/apps/gencmd/gencmd.c
Other filters: https://gist.github.com/wormyrocks/f7ca743b291a7791d571d08358e46fd0
*/

void sdtv_set_filtering(int enabled) {
  static VCHI_INSTANCE_T vchi_instance;
  static int initialized;
  static char buffer[256];
  VCHI_CONNECTION_T *vchi_connection = NULL;

  if (!initialized) {
    rt_log("initializing vchi\n");
    if (vchi_initialise(&vchi_instance) != 0) {
      return;
    }
    if (vchi_connect(NULL, 0, vchi_instance) != 0) {
      return;
    }
    initialized = 1;
  }
  rt_log("changing filtering\n");

  vc_vchi_gencmd_init(vchi_instance, &vchi_connection, 1);
  vc_gencmd_send(enabled ? FILTER_ON_CMD : FILTER_OFF_CMD);

  if ((vc_gencmd_read_response(buffer, sizeof(buffer) - 1)) != 0) {
    rt_log("gencmd response error\n");
  } else {
    rt_log("gencmd response: %s\n", buffer);
  }
  vc_gencmd_stop();
  // vchi_disconnect(vchi_instance);
}
