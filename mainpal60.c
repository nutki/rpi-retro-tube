#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <byteswap.h>
#include "mainlog.h"

#define BIT(x) (1<<(x))

#define VEC_REVID			0x100

#define VEC_CONFIG0			0x104
#define VEC_CONFIG0_YDEL_MASK		GENMASK(28, 26)
#define VEC_CONFIG0_YDEL(x)		((x) << 26)
#define VEC_CONFIG0_CDEL_MASK		GENMASK(25, 24)
#define VEC_CONFIG0_CDEL(x)		((x) << 24)
#define VEC_CONFIG0_PBPR_FIL		BIT(18)
#define VEC_CONFIG0_CHROMA_GAIN_MASK	GENMASK(17, 16)
#define VEC_CONFIG0_CHROMA_GAIN_UNITY	(0 << 16)
#define VEC_CONFIG0_CHROMA_GAIN_1_32	(1 << 16)
#define VEC_CONFIG0_CHROMA_GAIN_1_16	(2 << 16)
#define VEC_CONFIG0_CHROMA_GAIN_1_8	(3 << 16)
#define VEC_CONFIG0_CBURST_GAIN_MASK	GENMASK(14, 13)
#define VEC_CONFIG0_CBURST_GAIN_UNITY	(0 << 13)
#define VEC_CONFIG0_CBURST_GAIN_1_128	(1 << 13)
#define VEC_CONFIG0_CBURST_GAIN_1_64	(2 << 13)
#define VEC_CONFIG0_CBURST_GAIN_1_32	(3 << 13)
#define VEC_CONFIG0_CHRBW1		BIT(11)
#define VEC_CONFIG0_CHRBW0		BIT(10)
#define VEC_CONFIG0_SYNCDIS		BIT(9)
#define VEC_CONFIG0_BURDIS		BIT(8)
#define VEC_CONFIG0_CHRDIS		BIT(7)
#define VEC_CONFIG0_PDEN		BIT(6)
#define VEC_CONFIG0_YCDELAY		BIT(4)
#define VEC_CONFIG0_RAMPEN		BIT(2)
#define VEC_CONFIG0_YCDIS		BIT(2)
#define VEC_CONFIG0_STD_MASK		GENMASK(1, 0)
#define VEC_CONFIG0_NTSC_STD		0
#define VEC_CONFIG0_PAL_BDGHI_STD	1
#define VEC_CONFIG0_PAL_M_STD		2
#define VEC_CONFIG0_PAL_N_STD		3


#define VEC_CONFIG1			0x188
#define VEC_CONFIG_VEC_RESYNC_OFF	BIT(18)
#define VEC_CONFIG_RGB219		BIT(17)
#define VEC_CONFIG_CBAR_EN		BIT(16)
#define VEC_CONFIG_TC_OBB		BIT(15)
#define VEC_CONFIG1_OUTPUT_MODE_MASK	GENMASK(12, 10)
#define VEC_CONFIG1_C_Y_CVBS		(0 << 10)
#define VEC_CONFIG1_CVBS_Y_C		(1 << 10)
#define VEC_CONFIG1_PR_Y_PB		(2 << 10)
#define VEC_CONFIG1_RGB			(4 << 10)
#define VEC_CONFIG1_Y_C_CVBS		(5 << 10)
#define VEC_CONFIG1_C_CVBS_Y		(6 << 10)
#define VEC_CONFIG1_C_CVBS_CVBS		(7 << 10)
#define VEC_CONFIG1_DIS_CHR		BIT(9)
#define VEC_CONFIG1_DIS_LUMA		BIT(8)
#define VEC_CONFIG1_YCBCR_IN		BIT(6)
#define VEC_CONFIG1_DITHER_TYPE_LFSR	0
#define VEC_CONFIG1_DITHER_TYPE_COUNTER	BIT(5)
#define VEC_CONFIG1_DITHER_EN		BIT(4)
#define VEC_CONFIG1_CYDELAY		BIT(3)
#define VEC_CONFIG1_LUMADIS		BIT(2)
#define VEC_CONFIG1_COMPDIS		BIT(1)
#define VEC_CONFIG1_CUSTOM_FREQ		BIT(0)

#define PIXELVALVE2_VERTA 0x14
#define PIXELVALVE2_VERTB 0x18
#define PIXELVALVE2_VERTA_EVEN 0x1c
#define PIXELVALVE2_VERTB_EVEN 0x20

#define VEC_FREQ3_2			0x180
#define VEC_FREQ1_0			0x184

#define MAP_SIZE 4096UL
 
static unsigned int get_reg_base() {
    unsigned int reg;
    int f = open("/proc/device-tree/soc/ranges", O_RDONLY);
    read(f, &reg, sizeof(reg));
    read(f, &reg, sizeof(reg));
    reg = bswap_32(reg);
    rt_log("Detected register base address: 0x%08x\n", reg);
    return reg;
}
void engage_pal60(void) {
    int fd;
    if((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
        perror("Error opening /dev/mem (try sudo)");
        return;
    }
    static unsigned int reg_base;
    if (!reg_base) {
        reg_base = get_reg_base();
    }
    unsigned int *vec_regs = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, reg_base + 0x806000);
    rt_log("Videocore chip revision: %x\n", vec_regs[VEC_REVID/4]);
    int freq_pal_hi = 0x00002a09;
    int freq_pal_lo = 0x00008acb;
    vec_regs[VEC_FREQ3_2/4] = freq_pal_hi;
    vec_regs[VEC_FREQ1_0/4] = freq_pal_lo;
    vec_regs[VEC_CONFIG0/4] |= VEC_CONFIG0_PAL_M_STD;
    vec_regs[VEC_CONFIG1/4] |= VEC_CONFIG1_CUSTOM_FREQ;
    munmap(vec_regs, MAP_SIZE);
    close(fd);
}
