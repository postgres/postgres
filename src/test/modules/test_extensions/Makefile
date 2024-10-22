# src/test/modules/test_extensions/Makefile

MODULE = test_extensions
PGFILEDESC = "test_extensions - regression testing for EXTENSION support"

EXTENSION = test_ext1 test_ext2 test_ext3 test_ext4 test_ext5 test_ext6 \
            test_ext7 test_ext8 test_ext9 test_ext_cine test_ext_cor \
            test_ext_cyclic1 test_ext_cyclic2 \
            test_ext_extschema \
            test_ext_evttrig \
            test_ext_set_schema \
            test_ext_req_schema1 test_ext_req_schema2 test_ext_req_schema3

DATA = test_ext1--1.0.sql test_ext2--1.0.sql test_ext3--1.0.sql \
       test_ext4--1.0.sql test_ext5--1.0.sql test_ext6--1.0.sql \
       test_ext7--1.0.sql test_ext7--1.0--2.0.sql \
       test_ext7--2.0--2.1bad.sql test_ext7--2.0--2.2bad.sql \
       test_ext8--1.0.sql \
       test_ext9--1.0.sql \
       test_ext_cine--1.0.sql test_ext_cine--1.0--1.1.sql \
       test_ext_cor--1.0.sql \
       test_ext_cyclic1--1.0.sql test_ext_cyclic2--1.0.sql \
       test_ext_extschema--1.0.sql \
       test_ext_evttrig--1.0.sql test_ext_evttrig--1.0--2.0.sql \
       test_ext_set_schema--1.0.sql \
       test_ext_req_schema1--1.0.sql \
       test_ext_req_schema2--1.0.sql \
       test_ext_req_schema3--1.0.sql

REGRESS = test_extensions test_extdepend

# force C locale for output stability
NO_LOCALE = 1

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
