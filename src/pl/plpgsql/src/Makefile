#-------------------------------------------------------------------------
#
# Makefile for the PL/pgSQL procedural language
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

OBJS = \
	$(WIN32RES) \
	pl_comp.o \
	pl_exec.o \
	pl_funcs.o \
	pl_gram.o \
	pl_handler.o \
	pl_scanner.o

DATA = plpgsql.control plpgsql--1.0.sql

REGRESS_OPTS = --dbname=$(PL_TESTDB)

REGRESS = plpgsql_array plpgsql_cache plpgsql_call plpgsql_control \
	plpgsql_copy plpgsql_domain plpgsql_misc \
	plpgsql_record plpgsql_simple plpgsql_transaction \
	plpgsql_trap plpgsql_trigger plpgsql_varprops

# where to find gen_keywordlist.pl and subsidiary files
TOOLSDIR = $(top_srcdir)/src/tools
GEN_KEYWORDLIST = $(PERL) -I $(TOOLSDIR) $(TOOLSDIR)/gen_keywordlist.pl
GEN_KEYWORDLIST_DEPS = $(TOOLSDIR)/gen_keywordlist.pl $(TOOLSDIR)/PerfectHash.pm

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
pl_scanner.o: pl_reserved_kwlist_d.h pl_unreserved_kwlist_d.h

# See notes in src/backend/parser/Makefile about the following two rules
pl_gram.h: pl_gram.c
	touch $@

pl_gram.c: BISONFLAGS += -d

# generate plerrcodes.h from src/backend/utils/errcodes.txt
plerrcodes.h: $(top_srcdir)/src/backend/utils/errcodes.txt generate-plerrcodes.pl
	$(PERL) $(srcdir)/generate-plerrcodes.pl $< > $@

# generate keyword headers for the scanner
pl_reserved_kwlist_d.h: pl_reserved_kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --varname ReservedPLKeywords $<

pl_unreserved_kwlist_d.h: pl_unreserved_kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --varname UnreservedPLKeywords $<


check: submake
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)

installcheck: submake
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)


clean distclean: clean-lib
	rm -f $(OBJS)
	rm -rf $(pg_regress_clean_files)
	rm -f pl_gram.c pl_gram.h plerrcodes.h pl_reserved_kwlist_d.h pl_unreserved_kwlist_d.h
