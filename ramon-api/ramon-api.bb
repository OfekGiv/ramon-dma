SUMMARY = "libramon: user-space API library for /dev/ramon_dma (static)"
DESCRIPTION = "Core ioctl wrappers, DMA buffers, SpaceWire packet layer and SPFI stream layer over \
the ramon_dma driver. Installs libramon.a to ${libdir} and the headers to ${includedir}/ramon/. \
Customer recipes use DEPENDS = \"ramon-api\", -I${STAGING_INCDIR}/ramon and -lramon -lpthread."
SECTION = "PETALINUX/libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Must equal RAMON_API_VERSION in ramon_version.h.
PV = "0.1.0"

# ramon_dma_uapi.h comes from the module recipe's sysroot install.
DEPENDS = "ramon-dma"

SRC_URI = "file://Makefile \
           file://README.md \
           file://ramon_version.h \
           file://ramon.h \
           file://ramon_spw.h \
           file://ramon_spfi.h \
           file://ramon_priv.h \
           file://ramon_core.c \
           file://ramon_buf.c \
           file://ramon_axi.c \
           file://ramon_regs.c \
           file://ramon_crc.c \
           file://ramon_spw.c \
           file://ramon_spfi.c \
           file://ramon_selftest.c \
          "

S = "${WORKDIR}"

do_compile() {
    oe_runmake lib RAMON_UAPI_INC=${STAGING_INCDIR}/ramon
}

do_install() {
    oe_runmake install DESTDIR=${D} includedir=${includedir} libdir=${libdir} \
               RAMON_UAPI_INC=${STAGING_INCDIR}/ramon
}

# A static library: the runtime package is empty. The headers go to ${PN}-dev and
# libramon.a to ${PN}-staticdev (the default FILES cover both). The recipe sysroot is
# filled from ${D} regardless, so DEPENDS = "ramon-api" is all a consumer needs.
ALLOW_EMPTY:${PN} = "1"
