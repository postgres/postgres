#
# $Header: /cvsroot/pgsql/contrib/lo/Makefile,v 1.7 2000/07/09 13:13:14 petere Exp $
#

subdir = contrib/lo
top_builddir = ../..
include ../../src/Makefile.global

NAME	:= lo
SONAME	:= $(NAME)$(DLSUFFIX)

SQLS	= $(NAME).sql lo_drop.sql lo_test.sql
MODS	= $(NAME)$(DLSUFFIX)

CFLAGS += -I. $(CFLAGS_SL)

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

all: $(SONAME) $(NAME).sql

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(SONAME):g' < $< > $@

install: all installdirs
	$(INSTALL_SHLIB) $(SONAME)	$(libdir)/contrib
	$(INSTALL_DATA) README.$(NAME)	$(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql	$(datadir)/contrib
	$(INSTALL_DATA) lo_drop.sql	$(datadir)/contrib
	$(INSTALL_DATA) lo_test.sql	$(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(docdir)/contrib $(datadir)/contrib

uninstall:
	rm -f $(libdir)/contrib/$(SONAME) $(docdir)/contrib/README.$(NAME) \
	  $(addprefix $(datadir)/contrib/, $(NAME).sql lo_drop.sql lo_test.sql)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
