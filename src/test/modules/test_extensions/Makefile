# src/test/modules/test_extensions/Makefile

MODULE = test_extensions
PGFILEDESC = "test_extensions - regression testing for EXTENSION support"

EXTENSION = test_ext1 test_ext2 test_ext3 test_ext4 test_ext5 \
			test_ext_cyclic1 test_ext_cyclic2
DATA = test_ext1--1.0.sql test_ext2--1.0.sql test_ext3--1.0.sql \
	   test_ext4--1.0.sql test_ext5--1.0.sql test_ext_cyclic1--1.0.sql \
	   test_ext_cyclic2--1.0.sql

REGRESS = test_extensions

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_extensions
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
