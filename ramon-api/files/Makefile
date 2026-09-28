# libramon: static library over /dev/ramon_dma.
#
# Target build (ramon-api.bb):  make lib RAMON_UAPI_INC=${STAGING_INCDIR}/ramon
#                               make install DESTDIR=... includedir=... libdir=...
# Host check (x86_64):         make && make check
#
# CC, AR, CFLAGS and LDFLAGS come from the environment (Yocto exports them).

RAMON_UAPI_INC ?= ../../ramon-dma/files

override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -D_GNU_SOURCE -pthread -I. -I$(RAMON_UAPI_INC)

LIB  = libramon.a
HDRS = ramon_version.h ramon.h ramon_spw.h ramon_spfi.h
SRCS = ramon_core.c ramon_buf.c ramon_axi.c ramon_regs.c ramon_crc.c ramon_spw.c ramon_spfi.c
OBJS = $(SRCS:.c=.o)

all: lib
lib: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(OBJS): $(HDRS) ramon_priv.h $(wildcard $(RAMON_UAPI_INC)/ramon_dma_uapi.h)

ramon_selftest: ramon_selftest.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ramon_selftest.c $(LIB) -lpthread

check: ramon_selftest
	./ramon_selftest

DESTDIR    ?=
PREFIX     ?= /usr
includedir ?= $(PREFIX)/include
libdir     ?= $(PREFIX)/lib

install: $(LIB)
	install -d $(DESTDIR)$(includedir)/ramon $(DESTDIR)$(libdir)
	install -m 0644 $(HDRS) $(DESTDIR)$(includedir)/ramon/
	install -m 0644 $(LIB) $(DESTDIR)$(libdir)/

clean:
	rm -f $(OBJS) $(LIB) ramon_selftest

.PHONY: all lib check install clean
