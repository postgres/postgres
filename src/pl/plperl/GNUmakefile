# Makefile for PL/Perl
# $PostgreSQL: pgsql/src/pl/plperl/GNUmakefile,v 1.33 2007/12/01 15:30:09 adunstan Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(perl_useshrplib),true)
shared_libperl = yes
endif
ifeq ($(perl_useshrplib),yes)
shared_libperl = yes
endif

# If we don't have a shared library and the platform doesn't allow it
# to work without, we have to skip it.
ifneq (,$(findstring yes, $(shared_libperl)$(allow_nonpic_in_shlib)))

ifeq ($(PORTNAME), win32)
perl_archlibexp := $(subst \,/,$(perl_archlibexp))
perl_privlibexp := $(subst \,/,$(perl_privlibexp))
perl_embed_ldflags = -L$(perl_archlibexp)/CORE -lperl58
override CPPFLAGS += -DPLPERL_HAVE_UID_GID
# Perl on win32 contains /* within comment all over the header file,
# so disable this warning.
override CFLAGS += -Wno-comment
endif

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS) -I$(perl_archlibexp)/CORE

rpathdir = $(perl_archlibexp)/CORE


NAME = plperl
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0

OBJS = plperl.o spi_internal.o SPI.o

SHLIB_LINK = $(perl_embed_ldflags) $(BE_DLLLIBS)

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plperl
REGRESS = plperl plperl_trigger plperl_shared plperl_elog
# where to find psql for running the tests
PSQLDIR = $(bindir)

include $(top_srcdir)/src/Makefile.shlib


all: all-lib

SPI.c: SPI.xs
	$(PERL) $(perl_privlibexp)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@

# When doing a VPATH build, copy over the .sql and .out files so that the
# test script can find them.  See comments in src/test/regress/GNUmakefile.
ifdef VPATH

ifneq ($(PORTNAME),win32)
abs_srcdir := $(shell cd $(srcdir) && pwd)
abs_builddir := $(shell pwd)
else
abs_srcdir := $(shell cd $(srcdir) && pwd -W)
abs_builddir := $(shell pwd -W)
endif

test_files_src := $(wildcard $(srcdir)/sql/*.sql) $(wildcard $(srcdir)/expected/*.out)
test_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(test_files_src))

all: $(test_files_build)
$(test_files_build): $(abs_builddir)/%: $(srcdir)/%
	ln -s $< $@

endif

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(pkglibdir)/plperl$(DLSUFFIX)'
else
	@echo "*****"; \
	 echo "* PL/Perl was not installed due to lack of shared library support."; \
	 echo "*****"
endif

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)'

uninstall:
	rm -f '$(DESTDIR)$(pkglibdir)/plperl$(DLSUFFIX)'

installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress --psqldir=$(PSQLDIR) $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean maintainer-clean: clean-lib
	rm -f SPI.c $(OBJS)
	rm -rf results
	rm -f regression.diffs regression.out

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Perl because libperl is not a shared library."; \
	 echo "*** You might have to rebuild your Perl installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
