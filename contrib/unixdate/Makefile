#
# $Header: /cvsroot/pgsql/contrib/unixdate/Attic/Makefile,v 1.1 2000/06/15 19:05:22 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= unixdate

PROGRAM	= 
OBJS	= 
DOCS	= 
SQLS	= $(NAME).sql
BINS	=
EXAMPLES=
MODS	= 

all:: 

install: install_sql

install_sql:
	for inst_file in $(SQLS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_SQLDIR); \
	done

clean:
	$(RM) *~ 

