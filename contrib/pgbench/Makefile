#
# $Header: /cvsroot/pgsql/contrib/pgbench/Makefile,v 1.3 2000/06/16 18:59:07 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= pgbench

PROGRAM	= $(NAME)
OBJS	= $(NAME).o
DOCS	= README README.jis
SQLS	= 
BINS    = $(PROGRAM) 
EXAMPLES=
MODS	= 

CFLAGS += -I$(LIBPQDIR)

OTHER_CLEAN = 

all: $(PROGRAM)

$(PROGRAM): $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBPQ)

install: install_doc install_bin

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR)/$(DOCS).$(NAME); \
	done

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
