#
# $Header: /cvsroot/pgsql/contrib/mSQL-interface/Attic/Makefile,v 1.5 2000/08/31 16:09:00 petere Exp $
#

subdir = contrib/mSQL-interface
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= mpgsql
SO_MAJOR_VERSION := 0
SO_MINOR_VERSION := 0
OBJS	:= mpgsql.o

CFLAGS	+= -I$(libpq_srcdir)

include $(top_srcdir)/src/Makefile.shlib

all: all-lib

install: all installdirs install-lib
	$(INSTALL_DATA) README.$(NAME) $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir) $(docdir)/contrib

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
