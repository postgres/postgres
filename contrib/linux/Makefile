#
# $Header: /cvsroot/pgsql/contrib/linux/Attic/Makefile,v 1.2 2000/06/19 13:53:41 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= 

PROGRAM	= 
OBJS	= 
DOCS	= 
SQLS	= 
BINS	=
BINS    = postgres.init.csh postgres.init.sh

EXAMPLES=
MODS	= 

OTHER_CLEAN = 

all: 

install: install_bin

install_bin:
	for inst_file in $(BINS); do \
		$(INSTALL) $(INSTL_EXE_OPTS) $$inst_file $(CONTRIB_BINDIR); \
	done

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

clean:
	$(RM) *~ $(OBJS) $(MODS) $(PROGRAM) depend $(OTHER_CLEAN) core log

ifeq (depend,$(wildcard depend))
include depend
endif
