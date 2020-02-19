# Makefile for PL/Perl
# src/pl/plperl/GNUmakefile

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
override CPPFLAGS += -DPLPERL_HAVE_UID_GID
# Perl on win32 contains /* within comment all over the header file,
# so disable this warning.
override CPPFLAGS += -Wno-comment
endif

# Note: we need to include the perl_includespec directory last,
# probably because it sometimes contains some header files with names
# that clash with some of ours, or with some that we include, notably on
# Windows.
override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS) $(perl_embed_ccflags) $(perl_includespec)

# this is often, but not always, the same directory named by perl_includespec
rpathdir = $(perl_archlibexp)/CORE

PGFILEDESC = "PL/Perl - procedural language"

NAME = plperl

OBJS = plperl.o SPI.o Util.o $(WIN32RES)

DATA = plperl.control plperl--1.0.sql \
       plperlu.control plperlu--1.0.sql

PERLCHUNKS = plc_perlboot.pl plc_trusted.pl

# Perl on win32 ships with import libraries only for Microsoft Visual C++,
# which are not compatible with mingw gcc. Therefore we need to build a
# new import library to link with.
ifeq ($(PORTNAME), win32)

perlwithver := $(subst -l,,$(filter -l%, $(perl_embed_ldflags)))
PERLDLL := $(dir $(subst ',,$(PERL)))$(perlwithver).dll
# we no longer want to include the original -l spec in SHLIB_LINK
override perl_embed_ldflags :=

OBJS += lib$(perlwithver).a

lib$(perlwithver).a: $(perlwithver).def
	dlltool --dllname $(perlwithver).dll --def $(perlwithver).def --output-lib lib$(perlwithver).a

$(perlwithver).def: $(PERLDLL)
	pexports $^ > $@

endif # win32


SHLIB_LINK = $(perl_embed_ldflags)

REGRESS_OPTS = --dbname=$(PL_TESTDB)
REGRESS = plperl_setup plperl plperl_lc plperl_trigger plperl_shared \
	plperl_elog plperl_util plperl_init plperlu plperl_array \
	plperl_call plperl_transaction
# if Perl can support two interpreters in one backend,
# test plperl-and-plperlu cases
ifneq ($(PERL),)
ifeq ($(shell $(PERL) -V:usemultiplicity), usemultiplicity='define';)
	REGRESS += plperl_plperlu
endif
endif

# where to find xsubpp for building XS.
XSUBPPDIR = $(shell $(PERL) -e 'use List::Util qw(first); print first { -r "$$_/ExtUtils/xsubpp" } @INC')

include $(top_srcdir)/src/Makefile.shlib

plperl.o: perlchunks.h plperl_opmask.h plperl_helpers.h

plperl_opmask.h: plperl_opmask.pl
	@if [ x"$(perl_privlibexp)" = x"" ]; then echo "configure switch --with-perl was not specified."; exit 1; fi
	$(PERL) $< $@

perlchunks.h: $(PERLCHUNKS)
	@if [ x"$(perl_privlibexp)" = x"" ]; then echo "configure switch --with-perl was not specified."; exit 1; fi
	$(PERL) $(srcdir)/text2macro.pl --strip='^(\#.*|\s*)$$' $^ > $@

all: all-lib

%.c: %.xs
	@if [ x"$(perl_privlibexp)" = x"" ]; then echo "configure switch --with-perl was not specified."; exit 1; fi
# xsubpp -output option is required for coverage+vpath, but requires Perl 5.9.3
ifeq ($(enable_coverage)$(vpath_build),yesyes)
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap -output $@ $<
else
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@
endif


install: all install-lib install-data

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension' '$(DESTDIR)$(includedir_server)'

uninstall: uninstall-lib uninstall-data

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'
	$(INSTALL_DATA) $(srcdir)/plperl.h $(srcdir)/ppport.h $(srcdir)/plperl_helpers.h '$(DESTDIR)$(includedir_server)'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))
	rm -f $(addprefix '$(DESTDIR)$(includedir_server)'/, plperl.h ppport.h)

.PHONY: install-data uninstall-data


check: submake
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)

installcheck: submake
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean maintainer-clean: clean-lib
	rm -f SPI.c Util.c $(OBJS) perlchunks.h plperl_opmask.h
	rm -rf $(pg_regress_clean_files)
ifeq ($(PORTNAME), win32)
	rm -f $(perlwithver).def
endif
