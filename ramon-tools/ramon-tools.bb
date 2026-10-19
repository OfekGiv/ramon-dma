SUMMARY = "ramon_cli, ramon_test and ramon_accept: command line, automatic tests and board acceptance test for libramon / ramon_dma"
DESCRIPTION = "ramon_cli replaces the old dmaapi_testv2 shell (interactive with TAB completion, \
one-shot and script modes); ramon_test runs the SPW, SPFI, register, buffer, AXI and ZDMA tests; \
ramon_accept runs the board acceptance steps (RS-TOP, SYSMON, PL DDR4, SpaceWire, SPFI, NN services)."
SECTION = "PETALINUX/apps"
# linenoise.c/.h (the line editor) are BSD-2-Clause, everything else MIT.
LICENSE = "MIT & BSD-2-Clause"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
                    file://linenoise.c;beginline=13;endline=39;md5=679409940bac49bffd98c80aef73a3e2"

# Must equal RAMON_TOOLS_VERSION in tools_common.h.
PV = "0.4.5"

# libramon.a and its headers from ramon-api, ramon_dma_uapi.h from ramon-dma.
DEPENDS = "ramon-api ramon-dma"

SRC_URI = "file://Makefile \
           file://README.md \
           file://linenoise.c \
           file://linenoise.h \
           file://tools_common.h \
           file://tools_common.c \
           file://cli.h \
           file://cli_main.c \
           file://cli_core.c \
           file://cli_spw.c \
           file://cli_spfi.c \
           file://cli_misc.c \
           file://test.h \
           file://test_main.c \
           file://test_core.c \
           file://test_spw.c \
           file://test_spfi.c \
           file://test_soak.c \
           file://accept.h \
           file://accept_main.c \
           file://accept_board.c \
           file://accept_ddr4.c \
           file://accept_spw.c \
           file://accept_spfi.c \
           file://accept_nn.c \
           file://accept_limits.example \
          "

S = "${WORKDIR}"

do_compile() {
    oe_runmake all RAMON_UAPI_INC=${STAGING_INCDIR}/ramon RAMON_API_INC=${STAGING_INCDIR}/ramon \
                   RAMON_API_LIB=${STAGING_LIBDIR}
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ramon_cli ramon_test ramon_accept ${D}${bindir}/
    install -d ${D}${datadir}/ramon-tools
    install -m 0644 accept_limits.example ${D}${datadir}/ramon-tools/
}
