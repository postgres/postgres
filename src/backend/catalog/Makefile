#-------------------------------------------------------------------------
#
# Makefile for backend/catalog
#
# $Header: /cvsroot/pgsql/src/backend/catalog/Makefile,v 1.47 2003/08/01 16:12:32 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/catalog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = catalog.o dependency.o heap.o index.o indexing.o namespace.o aclchk.o \
       pg_aggregate.o pg_constraint.o pg_conversion.o pg_depend.o \
       pg_largeobject.o pg_namespace.o pg_operator.o pg_proc.o pg_type.o

BKIFILES = postgres.bki postgres.description

all: SUBSYS.o $(BKIFILES)

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

# Note: there are some undocumented dependencies on the ordering in which
# the catalog header files are assembled into postgres.bki.  In particular,
# indexing.h had better be last.

POSTGRES_BKI_SRCS := $(addprefix $(top_srcdir)/src/include/catalog/,\
	pg_proc.h pg_type.h pg_attribute.h pg_class.h \
	pg_attrdef.h pg_constraint.h pg_inherits.h pg_index.h \
	pg_operator.h pg_opclass.h pg_am.h pg_amop.h pg_amproc.h \
	pg_language.h pg_largeobject.h pg_aggregate.h pg_statistic.h \
	pg_rewrite.h pg_trigger.h pg_listener.h pg_description.h pg_cast.h \
	pg_namespace.h pg_conversion.h pg_database.h pg_shadow.h pg_group.h \
	pg_depend.h indexing.h \
    )

pg_includes := $(sort -I$(top_srcdir)/src/include -I$(top_builddir)/src/include)

# see explanation in ../parser/Makefile
postgres.description: postgres.bki ;

postgres.bki: genbki.sh $(POSTGRES_BKI_SRCS) \
    $(top_srcdir)/src/include/postgres_ext.h $(top_builddir)/src/include/pg_config_manual.h
	CPP='$(CPP)' AWK='$(AWK)' $(SHELL) $< $(BKIOPTS) -o postgres $(pg_includes) $(POSTGRES_BKI_SRCS) --set-version=$(VERSION)

.PHONY: install-data
install-data: $(BKIFILES) installdirs
	$(INSTALL_DATA) postgres.bki         $(DESTDIR)$(datadir)/postgres.bki
	$(INSTALL_DATA) postgres.description $(DESTDIR)$(datadir)/postgres.description
	$(INSTALL_DATA) $(srcdir)/information_schema.sql $(DESTDIR)$(datadir)/information_schema.sql
	$(INSTALL_DATA) $(srcdir)/sql_features.txt $(DESTDIR)$(datadir)/sql_features.txt

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(datadir)

.PHONY: uninstall-data
uninstall-data:
	rm -f $(addprefix $(DESTDIR)$(datadir)/, $(BKIFILES) information_schema.sql sql_features.txt)


clean: 
	rm -f SUBSYS.o $(OBJS) $(BKIFILES)
