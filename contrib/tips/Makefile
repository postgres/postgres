#
# $Header: /cvsroot/pgsql/contrib/tips/Attic/Makefile,v 1.1 2000/06/15 19:05:17 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= 

PROGRAM	= 
OBJS	= 
DOCS	= apachelog.doc
SQLS	= 
BINS	=
EXAMPLES=
MODS	= 

all::

install: install_doc 

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR); \
	done

clean:
	$(RM) *~ 

