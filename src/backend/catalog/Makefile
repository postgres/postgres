#-------------------------------------------------------------------------
#
# Makefile for backend/catalog
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/catalog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	aclchk.o \
	catalog.o \
	dependency.o \
	heap.o \
	index.o \
	indexing.o \
	namespace.o \
	objectaccess.o \
	objectaddress.o \
	partition.o \
	pg_aggregate.o \
	pg_attrdef.o \
	pg_cast.o \
	pg_class.o \
	pg_collation.o \
	pg_constraint.o \
	pg_conversion.o \
	pg_db_role_setting.o \
	pg_depend.o \
	pg_enum.o \
	pg_inherits.o \
	pg_largeobject.o \
	pg_namespace.o \
	pg_operator.o \
	pg_parameter_acl.o \
	pg_proc.o \
	pg_publication.o \
	pg_range.o \
	pg_shdepend.o \
	pg_subscription.o \
	pg_type.o \
	storage.o \
	toasting.o

include $(top_srcdir)/src/backend/common.mk

.PHONY: install-data
install-data: installdirs
	$(INSTALL_DATA) $(srcdir)/system_functions.sql '$(DESTDIR)$(datadir)/system_functions.sql'
	$(INSTALL_DATA) $(srcdir)/system_views.sql '$(DESTDIR)$(datadir)/system_views.sql'
	$(INSTALL_DATA) $(srcdir)/information_schema.sql '$(DESTDIR)$(datadir)/information_schema.sql'
	$(INSTALL_DATA) $(srcdir)/sql_features.txt '$(DESTDIR)$(datadir)/sql_features.txt'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

.PHONY: uninstall-data
uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)'/, system_functions.sql system_views.sql information_schema.sql sql_features.txt)
