#
# $Header: /cvsroot/pgsql/contrib/earthdistance/Makefile,v 1.2 2000/06/15 18:54:46 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= earthdistance

PROGRAM	= 
OBJS	= $(NAME).o
DOCS	= $(NAME).doc
SQLS	= $(NAME).sql
BINS    =
EXAMPLES=
MODS	= $(NAME)$(DLSUFFIX)

CFLAGS += -I. $(CFLAGS_SL)

OTHER_CLEAN = $(SQLS)

all: $(MODS) $(SQLS)

%.sql: %.sql.in
	$(SED) "s|MODULE_PATHNAME|$(CONTRIB_MODDIR)/$@|" < $< > $@

install: install_doc install_sql install_mod

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

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif
