
PGFILEDESC = "standalone json parser tester"
PGAPPICON = win32

TAP_TESTS = 1

OBJS = test_json_parser_incremental.o test_json_parser_perf.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_json_parser
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

.PHONY: clean

all: test_json_parser_incremental$(X) test_json_parser_perf$(X)

%.o: $(top_srcdir)/$(subdir)/%.c

PARSER_LIBS = $(top_builddir)/src/common/libpgcommon.a $(top_builddir)/src/port/libpgport.a

test_json_parser_incremental$(X): test_json_parser_incremental.o $(PARSER_LIBS)
	$(CC) $(CFLAGS) $^ -o $@

test_json_parser_perf$(X): test_json_parser_perf.o $(PARSER_LIBS)
	$(CC) $(CFLAGS) $^ -o $@

speed-check: test_json_parser_perf$(X)
	@echo Standard parser:
	time ./test_json_parser_perf 10000 $(top_srcdir)/$(subdir)/tiny.json
	@echo Incremental parser:
	time ./test_json_parser_perf -i 10000 $(top_srcdir)/$(subdir)/tiny.json

clean:
	rm -rf test_json_parser_perf test_json_parser_incremental tmp_check $(OBJS)
