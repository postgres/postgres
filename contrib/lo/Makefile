#
# $Header: /cvsroot/pgsql/contrib/lo/Makefile,v 1.4 2000/06/16 18:58:49 momjian Exp $
#

TOPDIR=../..

include ../Makefile.global

NAME	= lo

PROGRAM	= 
OBJS	= $(NAME).o
DOCS	= README
SQLS	= $(NAME).sql
BINS	=
EXAMPLES=
MODS	= $(NAME)$(DLSUFFIX)

CFLAGS += -I. $(CFLAGS_SL)

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

OTHER_CLEAN = $(SQLS)

all: $(MODS) $(SQLS)


%.sql: %.sql.in
	$(SED) "s|MODULE_PATHNAME|$(CONTRIB_MODDIR)/$@|" < $< > $@


install: install_doc install_sql install_mod

install_doc:
	for inst_file in $(DOCS); do \
		$(INSTALL) $(INSTL_LIB_OPTS) $$inst_file $(CONTRIB_DOCDIR)/$(DOCS).$(NAME); \
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

