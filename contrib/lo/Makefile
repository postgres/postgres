#
# $Header: /cvsroot/pgsql/contrib/lo/Makefile,v 1.8 2000/10/20 21:03:16 petere Exp $
#

subdir = contrib/lo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= lo
SONAME	:= $(NAME)$(DLSUFFIX)

SQLS	= $(NAME).sql lo_drop.sql lo_test.sql
MODS	= $(NAME)$(DLSUFFIX)

override CPPFLAGS += -I$(srcdir)
override CFLAGS += $(CFLAGS_SL)

ifdef REFINT_VERBOSE
override CPPFLAGS+= -DREFINT_VERBOSE
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
