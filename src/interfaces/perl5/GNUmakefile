# $Header: /cvsroot/pgsql/src/interfaces/perl5/Attic/GNUmakefile,v 1.7 2002/05/28 16:57:53 petere Exp $

subdir = src/interfaces/perl5
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# This would allow a non-root install of the Perl module, but it's not
# quite implemented yet.
ifeq ($(mysterious_feature),yes)
perl_installsitearch	= $(pkglibdir)
perl_installsitelib	= $(pkglibdir)
perl_installman3dir	= $(mandir)/man3
endif

override CPPFLAGS := -I$(libpq_srcdir) -I$(top_srcdir)/src/include $(CPPFLAGS) -I$(perl_archlibexp)/CORE
override CFLAGS += $(CFLAGS_SL)
override CPPFLAGS += -DXS_VERSION=\"$(shell sed -n "s/\$$.*::VERSION.*=.*'\(.*\)';/\1/p" $(srcdir)/Pg.pm)\"

# The code isn't clean with regard to these warnings.
ifeq ($(GCC),yes)
override CFLAGS := $(filter-out -Wall -Wmissing-declarations -Wmissing-prototypes, $(CFLAGS))
endif

POD2MAN = pod2man


NAME = Pg
OBJS = Pg.o
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
SHLIB_LINK = -L$(libpq_builddir) -lpq

include $(top_srcdir)/src/Makefile.shlib


all: all-lib Pg.pm Pg.bs auto/Pg/autosplit.ix Pg.$(perl_man3ext)

all-lib: libpq-all

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(libpq_builddir) all

Pg.c: Pg.xs typemap
	$(PERL) $(perl_privlibexp)/ExtUtils/xsubpp -typemap $(perl_privlibexp)/ExtUtils/typemap -typemap $(srcdir)/typemap $(srcdir)/Pg.xs >$@

auto/Pg/autosplit.ix: Pg.pm
	@$(mkinstalldirs) auto
	$(PERL) -MAutoSplit -e 'autosplit($$ARGV[0], $$ARGV[1], 0, 1, 1);' $< auto

Pg.bs:
	$(PERL) -MExtUtils::Mkbootstrap -e "Mkbootstrap('Pg', '');"
	touch $@

Pg.$(perl_man3ext): Pg.pm
	$(POD2MAN) $< $@


# During install, we must guard against the likelihood that we don't
# have permissions to install into the Perl module library.  It's not
# exactly fun to have to scan the build output, but...

install-warning-msg := { \
echo ""; \
echo "*** Skipping the installation of the Perl module for lack"; \
echo "*** of permissions.  To install it, change to the directory"; \
echo "***     `pwd`,"; \
echo "*** become the appropriate user, and enter '$(MAKE) install'."; \
echo ""; }

install: all installdirs
	@if test -w $(DESTDIR)$(perl_installsitearch); then \
	  $(INSTALL_DATA) Pg.pm $(DESTDIR)$(perl_installsitearch); \
	  $(INSTALL_DATA) Pg.bs $(DESTDIR)$(perl_installsitearch)/auto/Pg; \
	  $(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(perl_installsitearch)/auto/Pg/Pg$(DLSUFFIX); \
	  $(INSTALL_DATA) auto/Pg/autosplit.ix $(DESTDIR)$(perl_installsitearch)/auto/Pg; \
	  $(INSTALL_DATA) $(srcdir)/Pg.$(perl_man3ext) $(DESTDIR)$(perl_installman3dir); \
	else \
	  $(install-warning-msg); \
	fi

installdirs:
	-$(mkinstalldirs) $(DESTDIR)$(perl_installsitearch)/auto/Pg $(DESTDIR)$(perl_installman3dir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(perl_installsitearch)/, Pg.pm auto/Pg/Pg.bs auto/Pg/Pg$(DLSUFFIX) auto/Pg/autosplit.ix) $(DESTDIR)$(perl_installman3dir)/Pg.$(perl_man3ext)


clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) Pg.c Pg.bs Pg.$(perl_man3ext)
	rm -rf auto
