MODULE_big = sslinfo
OBJS = sslinfo.o
DATA_built = sslinfo.sql
DATA = uninstall_sslinfo.sql
DOCS = README.sslinfo

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/sslinfo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lssl -lcrypto -lssleay32 -leay32, $(LIBS))
