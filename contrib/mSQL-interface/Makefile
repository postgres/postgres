#
# $Header: /cvsroot/pgsql/contrib/mSQL-interface/Attic/Makefile,v 1.2 2000/06/16 18:58:54 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= mpgsql

PROGRAM	= 
OBJS	= 
DOCS	= README $(NAME).c
SQLS	= 
BINS    = 
EXAMPLES=
MODS	= 

OTHER_CLEAN = 

all::

install: install_doc

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR)/$(DOCS).$(NAME); \
	done

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif
