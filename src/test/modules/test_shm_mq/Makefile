# src/test/modules/test_shm_mq/Makefile

MODULE_big = test_shm_mq
OBJS = \
	$(WIN32RES) \
	setup.o \
	test.o \
	worker.o
PGFILEDESC = "test_shm_mq - example use of shared memory message queue"

EXTENSION = test_shm_mq
DATA = test_shm_mq--1.0.sql

REGRESS = test_shm_mq

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shm_mq
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
