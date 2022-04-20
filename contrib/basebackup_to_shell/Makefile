# contrib/basebackup_to_shell/Makefile

MODULE_big = basebackup_to_shell
OBJS = \
	$(WIN32RES) \
	basebackup_to_shell.o

PGFILEDESC = "basebackup_to_shell - target basebackup to shell command"

TAP_TESTS = 1

export GZIP_PROGRAM=$(GZIP)
export TAR

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/basebackup_to_shell
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
