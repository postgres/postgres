#-------------------------------------------------------------------------
#
# Makefile for src/include/catalog
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/include/catalog/Makefile
#
#-------------------------------------------------------------------------

subdir = src/include/catalog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# Note: the order of this list determines the order in which the catalog
# header files are assembled into postgres.bki.  BKI_BOOTSTRAP catalogs
# must appear first, and pg_statistic before pg_statistic_ext_data, and
# there are reputedly other, undocumented ordering dependencies.
CATALOG_HEADERS := \
	pg_proc.h \
	pg_type.h \
	pg_attribute.h \
	pg_class.h \
	pg_attrdef.h \
	pg_constraint.h \
	pg_inherits.h \
	pg_index.h \
	pg_operator.h \
	pg_opfamily.h \
	pg_opclass.h \
	pg_am.h \
	pg_amop.h \
	pg_amproc.h \
	pg_language.h \
	pg_largeobject_metadata.h \
	pg_largeobject.h \
	pg_aggregate.h \
	pg_statistic.h \
	pg_statistic_ext.h \
	pg_statistic_ext_data.h \
	pg_rewrite.h \
	pg_trigger.h \
	pg_event_trigger.h \
	pg_description.h \
	pg_cast.h \
	pg_enum.h \
	pg_namespace.h \
	pg_conversion.h \
	pg_depend.h \
	pg_database.h \
	pg_db_role_setting.h \
	pg_tablespace.h \
	pg_authid.h \
	pg_auth_members.h \
	pg_shdepend.h \
	pg_shdescription.h \
	pg_ts_config.h \
	pg_ts_config_map.h \
	pg_ts_dict.h \
	pg_ts_parser.h \
	pg_ts_template.h \
	pg_extension.h \
	pg_foreign_data_wrapper.h \
	pg_foreign_server.h \
	pg_user_mapping.h \
	pg_foreign_table.h \
	pg_policy.h \
	pg_replication_origin.h \
	pg_default_acl.h \
	pg_init_privs.h \
	pg_seclabel.h \
	pg_shseclabel.h \
	pg_collation.h \
	pg_parameter_acl.h \
	pg_partitioned_table.h \
	pg_range.h \
	pg_transform.h \
	pg_sequence.h \
	pg_publication.h \
	pg_publication_namespace.h \
	pg_publication_rel.h \
	pg_subscription.h \
	pg_subscription_rel.h

GENERATED_HEADERS := $(CATALOG_HEADERS:%.h=%_d.h)

POSTGRES_BKI_SRCS := $(addprefix $(top_srcdir)/src/include/catalog/, $(CATALOG_HEADERS))

# The .dat files we need can just be listed alphabetically.
POSTGRES_BKI_DATA = \
	pg_aggregate.dat \
	pg_am.dat \
	pg_amop.dat \
	pg_amproc.dat \
	pg_authid.dat \
	pg_cast.dat \
	pg_class.dat \
	pg_collation.dat \
	pg_conversion.dat \
	pg_database.dat \
	pg_language.dat \
	pg_namespace.dat \
	pg_opclass.dat \
	pg_operator.dat \
	pg_opfamily.dat \
	pg_proc.dat \
	pg_range.dat \
	pg_tablespace.dat \
	pg_ts_config.dat \
	pg_ts_config_map.dat \
	pg_ts_dict.dat \
	pg_ts_parser.dat \
	pg_ts_template.dat \
	pg_type.dat

GENBKI_OUTPUT_FILES = \
	$(GENERATED_HEADERS) \
	postgres.bki \
	system_constraints.sql \
	schemapg.h \
	syscache_ids.h \
	syscache_info.h \
	system_fk_info.h

all: generated-headers

.PHONY: generated-headers

generated-headers: bki-stamp

# bki-stamp records the last time we ran genbki.pl.  We don't rely on
# the timestamps of the individual output files, because the Perl script
# won't update them if they didn't change (to avoid unnecessary recompiles).
# Technically, this should depend on Makefile.global which supplies
# $(MAJORVERSION); but then genbki.pl would need to be re-run after every
# configure run, even in distribution tarballs.  So depending on configure.ac
# instead is cheating a bit, but it will achieve the goal of updating the
# version number when it changes.
bki-stamp: $(top_srcdir)/src/backend/catalog/genbki.pl $(top_srcdir)/src/backend/catalog/Catalog.pm $(POSTGRES_BKI_SRCS) $(POSTGRES_BKI_DATA) $(top_srcdir)/configure.ac $(top_srcdir)/src/include/access/transam.h
	$(PERL) $< --include-path=$(top_srcdir)/src/include/ \
		--set-version=$(MAJORVERSION) $(POSTGRES_BKI_SRCS)
	touch $@

install: all installdirs
	$(INSTALL_DATA) postgres.bki '$(DESTDIR)$(datadir)/postgres.bki'
	$(INSTALL_DATA) system_constraints.sql '$(DESTDIR)$(datadir)/system_constraints.sql'
# In non-vpath builds, src/include/Makefile already installs all headers.
ifeq ($(vpath_build),yes)
	$(INSTALL_DATA) schemapg.h '$(DESTDIR)$(includedir_server)'/catalog/schemapg.h
	$(INSTALL_DATA) syscache_ids.h '$(DESTDIR)$(includedir_server)'/catalog/syscache_ids.h
	$(INSTALL_DATA) system_fk_info.h '$(DESTDIR)$(includedir_server)'/catalog/system_fk_info.h
	for file in $(GENERATED_HEADERS); do \
	  $(INSTALL_DATA) $$file '$(DESTDIR)$(includedir_server)'/catalog/$$file || exit; \
	done
endif

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(includedir_server)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(datadir)'/, postgres.bki system_constraints.sql)
	rm -f $(addprefix '$(DESTDIR)$(includedir_server)'/catalog/, schemapg.h syscache_ids.h system_fk_info.h $(GENERATED_HEADERS))

clean:
	rm -f bki-stamp $(GENBKI_OUTPUT_FILES)

# 'make reformat-dat-files' is a convenience target for rewriting the
# catalog data files in our standard format.  This includes collapsing
# out any entries that are redundant with a BKI_DEFAULT annotation.
reformat-dat-files:
	$(PERL) $(srcdir)/reformat_dat_file.pl --output $(srcdir) $(srcdir)/pg_*.dat

# 'make expand-dat-files' is a convenience target for expanding out all
# default values in the catalog data files.  This should be run before
# altering or removing any BKI_DEFAULT annotation.
expand-dat-files:
	$(PERL) $(srcdir)/reformat_dat_file.pl --output $(srcdir) $(srcdir)/pg_*.dat --full-tuples

.PHONY: reformat-dat-files expand-dat-files
