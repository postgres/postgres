#
# $Header: /cvsroot/pgsql/contrib/noupdate/Attic/Makefile,v 1.6 2000/10/20 21:03:24 petere Exp $
#

subdir = contrib/noupdate
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= noup
SONAME	:= $(NAME)$(DLSUFFIX)

override CPPFLAGS += -I$(srcdir)
override CFLAGS += $(CFLAGS_SL)

all: $(SONAME) $(NAME).sql

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(SONAME):g' < $< > $@

install: all installdirs
	$(INSTALL_SHLIB) $(SONAME)	$(libdir)/contrib
	$(INSTALL_DATA) $(NAME).sql	$(datadir)/contrib
	$(INSTALL_DATA) README.$(NAME)	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(datadir)/contrib $(docdir)/contrib

uninstall:
	rm -f $(libdir)/contrib/$(SONAME) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
