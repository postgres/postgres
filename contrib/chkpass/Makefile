#
# $Header: /cvsroot/pgsql/contrib/chkpass/Makefile,v 1.1 2001/05/03 12:32:13 darcy Exp $
#

subdir = contrib/chkpass
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= chkpass
SONAME	:= $(NAME)$(DLSUFFIX)

SQLS	= $(NAME).sql
MODS	= $(NAME)$(DLSUFFIX)

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
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
	sed "s+%%PGDIR%%+$(libdir)+g" < chkpass.sql > $(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(docdir)/contrib $(datadir)/contrib

uninstall:
	rm -f $(libdir)/contrib/$(SONAME) $(docdir)/contrib/README.$(NAME) \
	  $(addprefix $(datadir)/contrib/, $(NAME).sql)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
