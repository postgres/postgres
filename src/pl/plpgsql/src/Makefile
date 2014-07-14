#-------------------------------------------------------------------------
#
# Makefile for the pl/pgsql procedural language
#
# src/pl/plpgsql/src/Makefile
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "PL/pgSQL - procedural language"

# Shared library parameters
NAME= plpgsql

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)
SHLIB_LINK = $(filter -lintl, $(LIBS))
rpath =

OBJS = pl_gram.o pl_handler.o pl_comp.o pl_exec.o \
	pl_funcs.o pl_scanner.o $(WIN32RES)

DATA = plpgsql.control plpgsql--1.0.sql plpgsql--unpackaged--1.0.sql

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


install: all install-lib install-data install-headers

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
	$(MKDIR_P) '$(DESTDIR)$(includedir_server)'

uninstall: uninstall-lib uninstall-data uninstall-headers

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'

# The plpgsql.h header file is needed by instrumentation plugins
install-headers: installdirs
	$(INSTALL_DATA) '$(srcdir)/plpgsql.h' '$(DESTDIR)$(includedir_server)'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))

uninstall-headers:
	rm -f '$(DESTDIR)$(includedir_server)/plpgsql.h'

.PHONY: install-data install-headers uninstall-data uninstall-headers


# Force these dependencies to be known even without dependency info built:
pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o pl_scanner.o: plpgsql.h pl_gram.h plerrcodes.h

# See notes in src/backend/parser/Makefile about the following two rules
pl_gram.h: pl_gram.c ;
pl_gram.c: BISONFLAGS += -d

# generate plerrcodes.h from src/backend/utils/errcodes.txt
plerrcodes.h: $(top_srcdir)/src/backend/utils/errcodes.txt generate-plerrcodes.pl
	$(PERL) $(srcdir)/generate-plerrcodes.pl $< > $@

distprep: pl_gram.h pl_gram.c plerrcodes.h

# pl_gram.c, pl_gram.h and plerrcodes.h are in the distribution tarball,
# so they are not cleaned here.
clean distclean: clean-lib
	rm -f $(OBJS)

maintainer-clean: clean
	rm -f pl_gram.c pl_gram.h plerrcodes.h
