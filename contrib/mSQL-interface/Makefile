#
# $Header: /cvsroot/pgsql/contrib/mSQL-interface/Attic/Makefile,v 1.3 2000/06/19 13:53:47 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= mpgsql

PROGRAM	= 
OBJS	= 
DOCS	= README.$(NAME) $(NAME).c
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
