#
# $Header: /cvsroot/pgsql/contrib/likeplanning/Attic/Makefile,v 1.1 2000/06/15 19:04:56 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= 

PROGRAM	= 
OBJS	= 
DOCS	= likeplanning.doc
SQLS	= disablelike.sql enablelike.sql
BINS	=
EXAMPLES=
MODS	= 

OTHER_CLEAN = 

all::

install: install_doc install_sql

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR); \
	done

install_sql:
	for inst_file in $(SQLS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_SQLDIR); \
	done

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif
