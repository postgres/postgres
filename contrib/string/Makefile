 #
# $Header: /cvsroot/pgsql/contrib/string/Attic/Makefile,v 1.14 2001/02/20 19:20:28 petere Exp $
#

subdir = contrib/string
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= string_io
SONAME	:= $(NAME)$(DLSUFFIX)

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)

all: $(SONAME) $(NAME).sql

%.sql: %.sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(@:.sql=$(DLSUFFIX)):g' < $< > $@

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
