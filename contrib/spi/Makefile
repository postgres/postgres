#
# $Header: /cvsroot/pgsql/contrib/spi/Makefile,v 1.13 2000/06/15 18:55:17 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= 

PROGRAM	= 
OBJS	= autoinc.o insert_username.o moddatetime.o refint.o timetravel.o
DOCS	= spi.doc
SQLS	= $(OBJS:.o=.sql)
BINS	=
EXAMPLES= $(OBJS:.o=.example)
MODS	= $(OBJS:.o=$(DLSUFFIX))

CFLAGS += -I. $(CFLAGS_SL)

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

OTHER_CLEAN = $(SQLS)

all: $(MODS) $(SQLS)

%.sql: %.sql.in
	$(SED) "s|MODULE_PATHNAME|$(CONTRIB_MODDIR)/$@|" < $< > $@

install: install_doc install_sql install_mod install_example

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR); \
	done

install_sql:
	for inst_file in $(SQLS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_SQLDIR); \
	done

install_mod:
	for inst_file in $(MODS); do \
		$(INSTALL) $(INSTL_SHLIB_OPTS) $$inst_file $(CONTRIB_MODDIR); \
	done

install_example:
	for inst_file in $(EXAMPLES); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_EXAMPLESDIR); \
	done

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif


