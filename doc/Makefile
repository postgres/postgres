#----------------------------------------------------------------------------
#
# PostgreSQL documentation installation makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/doc/Makefile,v 1.37 2009/06/27 21:06:46 tgl Exp $
#
#----------------------------------------------------------------------------

# This makefile is responsible for installing the documentation. The
# files to be installed are prepared specially and are placed in this
# directory during distribution bundling. In CVS-based trees these
# files don't exist, so we skip the installation in that case.
#
# Before we install the man pages, we massage the section numbers to
# follow the local conventions.
#
# To actually build the documenation, look into the src/ and src/sgml
# subdirectories.

subdir = doc
top_builddir = ..
include $(top_builddir)/src/Makefile.global

.NOTPARALLEL:

ifneq ($(wildcard $(srcdir)/postgres.tar.gz),)
found_html = yes
endif

ifneq ($(wildcard $(srcdir)/man.tar.gz),)
# SCO OpenServer's man system is sufficiently different to not bother.
ifneq ($(PORTNAME), sco)
found_man = yes
endif
endif


ifdef found_man
ifndef sqlmansect
sqlmansect = 7
endif
sqlmansectnum = $(shell expr X'$(sqlmansect)' : X'\([0-9]\)')

fix_sqlmansectnum = sed -e '/^\.TH/s/"7"/"$(sqlmansect)"/' \
			-e 's/\\fR(7)/\\fR($(sqlmansectnum))/g' \
			-e '1s/^\.so man7/.so man$(sqlmansectnum)/g;1s/^\(\.so.*\)\.7$$/\1.$(sqlmansect)/g'

all: man1/.timestamp man$(sqlmansectnum)/.timestamp

man1/.timestamp: man7/.timestamp
	@echo timestamp >$@

man7/.timestamp: man.tar.gz
	gzip -d -c $< | $(TAR) xf -
ifneq ($(sqlmansectnum),7)
	for file in man1/*.1; do \
	  mv $$file $$file.bak && \
	  $(fix_sqlmansectnum) $$file.bak >$$file && \
	  rm -f $$file.bak || exit; \
	done
endif
	@echo timestamp >$@

ifneq ($(sqlmansectnum),7)
man$(sqlmansectnum)/.timestamp: man7/.timestamp
	$(mkinstalldirs) man$(sqlmansectnum)
	for file in man7/*.7; do \
	  $(fix_sqlmansectnum) $$file >man$(sqlmansectnum)/`basename $$file | sed 's/.7$$/.$(sqlmansect)/'` || exit; \
	done
	@echo timestamp >$@
endif
endif # found_man


install: all installdirs
ifdef found_html
	gzip -d -c $(srcdir)/postgres.tar.gz | ( cd $(DESTDIR)$(htmldir)/html && $(TAR) xf - )
endif
ifdef found_man
	for file in man1/*.1 man$(sqlmansectnum)/*.$(sqlmansect) ; do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(mandir)/$$file || exit; \
	done
endif


installdirs:
ifdef found_html
	$(mkinstalldirs) '$(DESTDIR)$(htmldir)'/html
endif
ifdef found_man
	$(mkinstalldirs) $(addprefix '$(DESTDIR)$(mandir)'/man, 1 $(sqlmansectnum))
endif


uninstall:
ifdef found_html
	rm -f $(addprefix $(DESTDIR)$(htmldir)/html/, $(shell gunzip -c $(srcdir)/postgres.tar.gz | $(TAR) tf -))
endif
ifdef found_man
	rm -f $(filter-out %/, $(addprefix $(DESTDIR)$(mandir)/, $(shell gunzip -c $(srcdir)/man.tar.gz | $(TAR) tf - | sed -e 's,man7/,man$(sqlmansectnum)/,' -e 's/.7$$/.$(sqlmansect)/')))
endif


clean distclean maintainer-clean:
	rm -rf man1/ man7/ man$(sqlmansectnum)/
	$(MAKE) -C src $@
