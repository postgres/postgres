# contrib/test_shm_mq/Makefile

MODULE_big = test_shm_mq
OBJS = test.o setup.o worker.o

EXTENSION = test_shm_mq
DATA = test_shm_mq--1.0.sql

REGRESS = test_shm_mq

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_shm_mq
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
