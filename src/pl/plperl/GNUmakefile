# Makefile for PL/Perl
# src/pl/plperl/GNUmakefile

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(perl_useshrplib),true)
shared_libperl = yes
endif
ifeq ($(perl_useshrplib),yes)
shared_libperl = yes
endif

# If we don't have a shared library, we have to skip it.
ifeq ($(shared_libperl),yes)

ifeq ($(PORTNAME), win32)
override CPPFLAGS += -DPLPERL_HAVE_UID_GID
# Perl on win32 contains /* within comment all over the header file,
# so disable this warning.
override CFLAGS += -Wno-comment
endif

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS) -I$(perl_archlibexp)/CORE

rpathdir = $(perl_archlibexp)/CORE


NAME = plperl

OBJS = plperl.o SPI.o Util.o

DATA = plperl.control plperl--1.0.sql plperl--unpackaged--1.0.sql \
       plperlu.control plperlu--1.0.sql plperlu--unpackaged--1.0.sql

PERLCHUNKS = plc_perlboot.pl plc_trusted.pl

SHLIB_LINK = $(perl_embed_ldflags)

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-extension=plperl  --load-extension=plperlu
REGRESS = plperl plperl_lc plperl_trigger plperl_shared plperl_elog plperl_util plperl_init plperlu plperl_array
# if Perl can support two interpreters in one backend,
# test plperl-and-plperlu cases
ifneq ($(PERL),)
ifeq ($(shell $(PERL) -V:usemultiplicity), usemultiplicity='define';)
	REGRESS += plperl_plperlu
endif
endif
# where to find psql for running the tests
PSQLDIR = $(bindir)

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

SPI.c: SPI.xs plperl_helpers.h
	@if [ x"$(perl_privlibexp)" = x"" ]; then echo "configure switch --with-perl was not specified."; exit 1; fi
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@

Util.c: Util.xs plperl_helpers.h
	@if [ x"$(perl_privlibexp)" = x"" ]; then echo "configure switch --with-perl was not specified."; exit 1; fi
	$(PERL) $(XSUBPPDIR)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@


install: all install-lib install-data

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'

uninstall: uninstall-lib uninstall-data

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))

.PHONY: install-data uninstall-data


check: all submake
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)

installcheck: submake
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean maintainer-clean: clean-lib
	rm -f SPI.c Util.c $(OBJS) perlchunks.h plperl_opmask.h
	rm -rf $(pg_regress_clean_files)

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Perl because libperl is not a shared library."; \
	 echo "*** You might have to rebuild your Perl installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
