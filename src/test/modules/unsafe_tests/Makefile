# src/test/modules/unsafe_tests/Makefile

REGRESS = rolenames setconfig alter_system_table guc_privs
REGRESS_OPTS = \
	--create-role=regress_authenticated_user_db_sr \
	--create-role=regress_authenticated_user_db_ssa \
	--create-role=regress_authenticated_user_sr \
	--create-role=regress_authenticated_user_ssa

# the whole point of these tests is to not run installcheck
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/unsafe_tests
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
