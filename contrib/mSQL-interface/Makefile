#
# $Header: /cvsroot/pgsql/contrib/mSQL-interface/Attic/Makefile,v 1.1 2000/06/15 19:05:03 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= mpgsql

PROGRAM	= 
OBJS	= 
DOCS	= $(NAME).doc $(NAME).c
SQLS	= 
BINS    = 
EXAMPLES=
MODS	= 

OTHER_CLEAN = 

all::

install: install_doc

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR); \
	done

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif
