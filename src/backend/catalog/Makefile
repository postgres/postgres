#-------------------------------------------------------------------------
#
# Makefile for catalog
#
# $Header: /cvsroot/pgsql/src/backend/catalog/Makefile,v 1.29 2000/10/21 15:55:21 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/catalog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = catalog.o heap.o index.o indexing.o aclchk.o \
       pg_aggregate.o pg_largeobject.o pg_operator.o pg_proc.o \
       pg_type.o

BKIFILES = global.bki template1.bki global.description template1.description

all: SUBSYS.o $(BKIFILES)

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)


GLOBAL_BKI_SRCS := $(addprefix $(top_srcdir)/src/include/catalog/,\
	pg_database.h pg_variable.h pg_shadow.h pg_group.h pg_log.h \
    )

TEMPLATE1_BKI_SRCS := $(addprefix $(top_srcdir)/src/include/catalog/,\
	pg_proc.h pg_type.h pg_attribute.h pg_class.h \
	pg_inherits.h pg_index.h pg_statistic.h \
	pg_operator.h pg_opclass.h pg_am.h pg_amop.h pg_amproc.h \
	pg_language.h pg_largeobject.h \
	pg_aggregate.h pg_ipl.h pg_inheritproc.h \
	pg_rewrite.h pg_listener.h pg_description.h indexing.h \
    )

pg_includes := $(sort -I$(top_srcdir)/src/include -I$(top_builddir)/src/include)

global.bki global.description: genbki.sh $(GLOBAL_BKI_SRCS) $(top_srcdir)/src/include/catalog/indexing.h \
    $(top_srcdir)/src/include/postgres_ext.h $(top_builddir)/src/include/config.h
	CPP='$(CPP)' AWK='$(AWK)' $(SHELL) $< $(BKIOPTS) -o global $(pg_includes) $(GLOBAL_BKI_SRCS)

template1.bki template1.description: genbki.sh $(TEMPLATE1_BKI_SRCS) \
    $(top_srcdir)/src/include/postgres_ext.h $(top_builddir)/src/include/config.h
	CPP='$(CPP)' AWK='$(AWK)' $(SHELL) $< $(BKIOPTS) -o template1 $(pg_includes) $(TEMPLATE1_BKI_SRCS)

.PHONY: install-bki
install-bki: $(BKIFILES) installdirs
	$(INSTALL_DATA) global.bki            $(DESTDIR)$(datadir)/global.bki
	$(INSTALL_DATA) global.description    $(DESTDIR)$(datadir)/global.description
	$(INSTALL_DATA) template1.bki         $(DESTDIR)$(datadir)/template1.bki
	$(INSTALL_DATA) template1.description $(DESTDIR)$(datadir)/template1.description

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(datadir)

.PHONY: uninstall-bki
uninstall-bki:
	rm -f $(addprefix $(DESTDIR)$(datadir)/, $(BKIFILES))


clean: 
	rm -f SUBSYS.o $(OBJS) $(BKIFILES)


depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
