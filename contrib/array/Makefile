#
# $Header: /cvsroot/pgsql/contrib/array/Attic/Makefile,v 1.13 2000/10/20 21:03:03 petere Exp $
#

subdir = contrib/array
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= array_iterator
SONAME	:= $(NAME)$(DLSUFFIX)

override CPPFLAGS += -I$(srcdir)
override CFLAGS += $(CFLAGS_SL)

all: $(SONAME) $(NAME).sql

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(SONAME):g' < $< > $@

install: all installdirs
	$(INSTALL_DATA) README.$(NAME)	$(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql	$(datadir)/contrib
	$(INSTALL_SHLIB) $(SONAME)	$(libdir)/contrib

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
