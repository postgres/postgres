
PGFILEDESC = "standalone json parser tester"
PGAPPICON = win32

TAP_TESTS = 1

OBJS = test_json_parser_incremental.o test_json_parser_perf.o $(WIN32RES)

EXTRA_CLEAN = test_json_parser_incremental$(X) test_json_parser_incremental_shlib$(X) test_json_parser_perf$(X)

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

all: test_json_parser_incremental$(X) test_json_parser_incremental_shlib$(X) test_json_parser_perf$(X)

%.o: $(top_srcdir)/$(subdir)/%.c

test_json_parser_incremental$(X): test_json_parser_incremental.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@

test_json_parser_incremental_shlib$(X): test_json_parser_incremental.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -lpgcommon_excluded_shlib $(libpq_pgport_shlib) $(filter -lintl, $(LIBS)) -o $@

test_json_parser_perf$(X): test_json_parser_perf.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@

speed-check: test_json_parser_perf$(X)
	@echo Standard parser:
	time ./test_json_parser_perf 10000 $(top_srcdir)/$(subdir)/tiny.json
	@echo Incremental parser:
	time ./test_json_parser_perf -i 10000 $(top_srcdir)/$(subdir)/tiny.json
