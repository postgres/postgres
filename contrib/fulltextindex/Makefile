#
# $Header: /cvsroot/pgsql/contrib/fulltextindex/Attic/Makefile,v 1.5 2000/06/19 13:53:30 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= fti

PROGRAM	= 
OBJS	= $(NAME).o
DOCS	= README.$(NAME)
SQLS	= $(NAME).sql
BINS	= fti.pl
EXAMPLES=
MODS	= $(NAME)$(DLSUFFIX)

CFLAGS += -I. $(CFLAGS_SL)

OTHER_CLEAN = $(SQLS)

all: $(MODS) $(SQLS)

%.sql: %.sql.in
	$(SED) "s|MODULE_PATHNAME|$(CONTRIB_MODDIR)/$@|" < $< > $@

install: install_doc install_sql install_mod install_bin

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
