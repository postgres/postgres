#
# $Header: /cvsroot/pgsql/contrib/userlock/Attic/Makefile,v 1.13 2000/10/20 21:03:34 petere Exp $
#

subdir = contrib/userlock
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= user_locks
SONAME	:= $(NAME)$(DLSUFFIX)

override CPPFLAGS += -I$(srcdir)
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
	rm -rf $(libdir)/contrib/$(SONAME) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
