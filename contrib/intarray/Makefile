# $Header: /cvsroot/pgsql/contrib/intarray/Makefile,v 1.2 2001/01/13 02:18:31 petere Exp $

subdir = contrib/intarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# override libdir to install shlib in contrib not main directory
libdir := $(libdir)/contrib

# shared library parameters
NAME= _int
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

override CPPFLAGS += -I$(srcdir) -DPGSQL71

OBJS= _int.o

all: all-lib $(NAME).sql

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


$(NAME).sql: $(NAME).sql.in
	sed 's,MODULE_PATHNAME,$(libdir)/$(shlib),g' $< >$@

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress

installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress _int

check:
	@echo "'$(MAKE) check' is not supported."
	@echo "Do '$(MAKE) install', then '$(MAKE) installcheck' instead."

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/README.intarray $(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql $(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.intarray $(datadir)/contrib/$(NAME).sql

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(NAME).sql
# things created by various check targets
	rm -rf results
	rm -f regression.diffs regression.out
