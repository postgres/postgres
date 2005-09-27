#
# $PostgreSQL: pgsql/contrib/mSQL-interface/Makefile,v 1.10 2005/09/27 17:13:05 tgl Exp $
#

NAME	= mpgsql
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS	= mpgsql.o

PG_CPPFLAGS = -I$(libpq_srcdir)

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/mSQL-interface
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

all: all-lib

install: all installdirs install-lib
	$(INSTALL_DATA) README.$(NAME) $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir) $(docdir)/contrib

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
