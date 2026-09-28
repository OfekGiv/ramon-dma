SUMMARY = "ramon_dma: AXI DMA, ZDMA, SpaceWire and SPFI driver for the ramon board"
DESCRIPTION = "Replaces axidmasgk and ps2psk. Provides /dev/ramon_dma and installs the \
userspace ABI header to ${includedir}/ramon/ramon_dma_uapi.h."
SECTION = "PETALINUX/modules"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=12f884d2ae1ff87c09e5b7ccc2c4ca7e"

inherit module

# Tracks MODULE_VERSION in ramon_dma.h (0.<implementation step>.0 during the rewrite).
PV = "0.8.6"

INHIBIT_PACKAGE_STRIP = "1"

# Sources are added here as each implementation step lands.
SRC_URI = "file://Makefile \
           file://COPYING \
           file://ramon_dma_uapi.h \
           file://ramon_dma.h \
           file://ramon_board.h \
           file://ramon_core.c \
           file://ramon_of.c \
           file://ramon_buf.c \
           file://ramon_regwin.c \
           file://ramon_axidma.c \
           file://ramon_zdma.c \
           file://ramon_spw.c \
           file://ramon_spfi.c \
          "

S = "${WORKDIR}"

# Extra compiler warnings; they show up in log.do_compile (bitbake does not print them).
EXTRA_OEMAKE += "W=1"

# Load automatically at boot. The old axidmasg_drv binds the same device-tree node,
# so the old axidmasgk and ps2psk modules must not be in the same image.
KERNEL_MODULE_AUTOLOAD += "ramon_dma"

# The ABI header for userspace recipes (DEPENDS = "ramon-dma", -I${STAGING_INCDIR}/ramon).
# It lands in the sysroot and in the ${PN}-dev package.
do_install:append() {
    install -d ${D}${includedir}/ramon
    install -m 0644 ${S}/ramon_dma_uapi.h ${D}${includedir}/ramon/
}
