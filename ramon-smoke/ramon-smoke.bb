SUMMARY = "ramon_smoke: on-target smoke test for /dev/ramon_dma"
SECTION = "PETALINUX/apps"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=12f884d2ae1ff87c09e5b7ccc2c4ca7e"

# Must equal SMOKE_VERSION in ramon_smoke.c (0.<driver step>.<tool revision>).
PV = "0.8.8"

# ramon_dma_uapi.h comes from the module recipe's sysroot install.
DEPENDS = "ramon-dma"

SRC_URI = "file://ramon_smoke.c \
           file://COPYING \
          "

S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -I${STAGING_INCDIR}/ramon \
        -o ramon_smoke ramon_smoke.c -lpthread
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ramon_smoke ${D}${bindir}/
}
