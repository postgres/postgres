#
# $Header: /cvsroot/pgsql/contrib/chkpass/Makefile,v 1.2 2001/08/23 16:50:33 tgl Exp $
#

subdir = contrib/chkpass
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# override libdir to install shlib in contrib not main directory
libdir := $(libdir)/contrib

# shared library parameters
NAME= chkpass
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

ifdef REFINT_VERBOSE
override CPPFLAGS+= -DREFINT_VERBOSE
endif

OBJS= chkpass.o

all: all-lib $(NAME).sql

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/$(shlib):g' < $< > $@

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/README.$(NAME)  $(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql $(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.$(NAME) $(datadir)/contrib/$(NAME).sql

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
