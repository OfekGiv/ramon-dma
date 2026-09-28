# ramon_cli and ramon_test, linked against libramon.a.
#
# Target build (ramon-tools.bb):
#   make RAMON_UAPI_INC=${STAGING_INCDIR}/ramon RAMON_API_INC=${STAGING_INCDIR}/ramon \
#        RAMON_API_LIB=${STAGING_LIBDIR}
# Host check (x86_64, after "make -C ../../ramon-api/files"): make

RAMON_UAPI_INC ?= ../../ramon-dma/files
RAMON_API_INC  ?= ../../ramon-api/files
RAMON_API_LIB  ?= ../../ramon-api/files

override CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -D_GNU_SOURCE -pthread \
	   -I. -I$(RAMON_API_INC) -I$(RAMON_UAPI_INC)
override LDFLAGS += -L$(RAMON_API_LIB)
override LDLIBS  += -lramon -lpthread

COMMON    = tools_common.o
CLI_OBJS  = cli_main.o cli_core.o cli_spw.o cli_spfi.o cli_misc.o linenoise.o
TEST_OBJS = test_main.o test_core.o test_spw.o test_spfi.o test_soak.o

all: ramon_cli ramon_test

ramon_cli: $(CLI_OBJS) $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

ramon_test: $(TEST_OBJS) $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# vendored upstream code: its own warning level
linenoise.o: linenoise.c linenoise.h
	$(CC) $(filter-out -Wpedantic -Wshadow,$(CFLAGS)) -Wno-unused-parameter -c -o $@ $<

$(CLI_OBJS) $(TEST_OBJS) $(COMMON): tools_common.h
$(CLI_OBJS): cli.h
$(TEST_OBJS): test.h

clean:
	rm -f *.o ramon_cli ramon_test

.PHONY: all clean
