# Makefile for PL/Perl
# $PostgreSQL: pgsql/src/pl/plperl/GNUmakefile,v 1.18 2004/11/19 19:22:58 tgl Exp $

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
perl_embed_ldflags := -L $(perl_archlibexp)/CORE -lperl58
override CPPFLAGS += -DPLPERL_HAVE_UID_GID
endif

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS) -I$(perl_archlibexp)/CORE

rpathdir = $(perl_archlibexp)/CORE


NAME = plperl
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0

OBJS = plperl.o spi_internal.o SPI.o

SHLIB_LINK = $(perl_embed_ldflags) $(BE_DLLLIBS)

include $(top_srcdir)/src/Makefile.shlib


all: all-lib

SPI.c: SPI.xs
	$(PERL) $(perl_privlibexp)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap $< >$@

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(pkglibdir)/plperl$(DLSUFFIX)
else
	@echo "*****"; \
	 echo "* PL/Perl was not installed due to lack of shared library support."; \
	 echo "*****"
endif

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)

uninstall:
	rm -f $(DESTDIR)$(pkglibdir)/plperl$(DLSUFFIX)

clean distclean maintainer-clean: clean-lib
	rm -f SPI.c $(OBJS)

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Perl because libperl is not a shared library."; \
	 echo "*** You might have to rebuild your Perl installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
