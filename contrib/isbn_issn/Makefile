#
# $Header: /cvsroot/pgsql/contrib/isbn_issn/Attic/Makefile,v 1.9 2001/02/20 19:20:27 petere Exp $
#

subdir = contrib/isbn_issn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= isbn_issn
SONAME	:= $(NAME)$(DLSUFFIX)

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)

all: $(SONAME) $(NAME).sql

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(SONAME):g' < $< > $@

install: all installdirs
	$(INSTALL_DATA) README.$(NAME)  $(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql $(datadir)/contrib
	$(INSTALL_SHLIB) $(SONAME)  $(libdir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)/contrib

uninstall:
	rm -rf $(docdir)/contrib/README.$(NAME) $(datadir)/contrib/$(NAME).sql $(libdir)/contrib/$(SONAME)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
