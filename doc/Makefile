#----------------------------------------------------------------------------
#
# PostgreSQL documentation installation makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/doc/Makefile,v 1.29 2007/02/09 15:55:57 petere Exp $
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
ifneq (,$(docdir))
found_html = yes
endif
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

all: man1/.timestamp man$(sqlmansectnum)/.timestamp

man1/.timestamp: man$(sqlmansect_dummy)/.timestamp
	@echo timestamp >$@

man$(sqlmansect_dummy)/.timestamp: man.tar.gz
	gzip -d -c $< | $(TAR) xf -
	for file in man1/*.1; do \
	  mv $$file $$file.bak && \
	  sed -e 's/\\fR($(sqlmansect_dummy))/\\fR($(sqlmansectnum))/' $$file.bak >$$file && \
	  rm -f $$file.bak || exit; \
	done
	@echo timestamp >$@

man$(sqlmansectnum)/.timestamp: man$(sqlmansect_dummy)/.timestamp
	$(mkinstalldirs) man$(sqlmansectnum)
	for file in man$(sqlmansect_dummy)/*.$(sqlmansect_dummy); do \
	  sed -e '/^\.TH/s/"$(sqlmansect_dummy)"/"$(sqlmansect)"/'   \
              -e 's/\\fR($(sqlmansect_dummy))/\\fR($(sqlmansectnum))/' \
	      $$file >man$(sqlmansectnum)/`basename $$file | sed 's/.$(sqlmansect_dummy)$$/.$(sqlmansect)/'` || exit; \
	done
	@echo timestamp >$@
endif # found_man


install: all installdirs
ifdef found_html
	gzip -d -c $(srcdir)/postgres.tar.gz | ( cd $(DESTDIR)$(docdir)/html && $(TAR) xf - )
endif
ifdef found_man
	for file in man1/*.1 man$(sqlmansectnum)/*.$(sqlmansect) ; do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(mandir)/$$file || exit; \
	done
endif


installdirs:
ifdef found_html
	$(mkinstalldirs) '$(DESTDIR)$(docdir)'/html
endif
ifdef found_man
	$(mkinstalldirs) $(addprefix '$(DESTDIR)$(mandir)'/man, 1 $(sqlmansectnum))
endif


uninstall:
ifdef found_html
	-rm -f $(addprefix $(DESTDIR)$(docdir)/html/, $(shell gunzip -c $(srcdir)/postgres.tar.gz | tar tf -))
endif
ifdef found_man
	-rm -f $(addprefix $(DESTDIR)$(mandir)/, $(shell gunzip -c $(srcdir)/man.tar.gz | tar tf - | sed -e 's,man$(sqlmansect_dummy)/,man$(sqlmansectnum)/,' -e 's/.$(sqlmansect_dummy)$$/.$(sqlmansect)/'))
endif


clean distclean maintainer-clean:
	rm -rf man1/ man$(sqlmansectnum)/ man$(sqlmansect_dummy)/
	$(MAKE) -C src $@
