# Copyright (c) 2022-2024, PostgreSQL Global Development Group

# ldap_password_func Makefile

export with_ldap

MODULE_big = ldap_password_func
OBJS = ldap_password_func.o $(WIN32RES)
PGFILEDESC = "set hook to mutate ldapbindpasswd"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/ldap_password_func
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
