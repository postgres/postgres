#
# PostgreSQL top level makefile
#
# $Header: /cvsroot/pgsql/GNUmakefile.in,v 1.35.2.1 2004/06/13 21:50:04 petere Exp $
#

subdir =
top_builddir = .
include $(top_builddir)/src/Makefile.global

all:
	$(MAKE) -C doc all
	$(MAKE) -C src all
	@echo "All of PostgreSQL successfully made. Ready to install."

install:
	$(MAKE) -C doc install
	$(MAKE) -C src install
	@echo "PostgreSQL installation complete."

installdirs uninstall distprep:
	$(MAKE) -C doc $@
	$(MAKE) -C src $@

install-all-headers:
	$(MAKE) -C src $@

# clean, distclean, etc should apply to contrib too, even though
# it's not built by default
clean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C src $@
# Garbage from autoconf:
	@rm -rf autom4te.cache/

# Important: distclean `src' last, otherwise Makefile.global
# will be gone too soon.
distclean maintainer-clean:
	-$(MAKE) -C doc $@
	-$(MAKE) -C contrib $@
	-$(MAKE) -C src $@
	-rm -f config.cache config.log config.status GNUmakefile
# Garbage from autoconf:
	@rm -rf autom4te.cache/

check: all

check installcheck:
	$(MAKE) -C src/test $@

GNUmakefile: GNUmakefile.in $(top_builddir)/config.status
	./config.status $@


##########################################################################

distdir	:= postgresql-$(VERSION)
dummy	:= =install=
garbage := =*  "#"*  ."#"*  *~*  *.orig  *.rej  core  postgresql-*

dist: $(distdir).tar.gz 
ifeq ($(split-dist), yes)
dist: postgresql-base-$(VERSION).tar.gz postgresql-docs-$(VERSION).tar.gz postgresql-opt-$(VERSION).tar.gz postgresql-test-$(VERSION).tar.gz 
endif
dist:
	-rm -rf $(distdir)

$(distdir).tar: distdir
	$(TAR) chf $@ $(distdir)

opt_files := \
	src/tools src/corba src/tutorial \
	src/bin/pgtclsh \
	$(addprefix src/interfaces/, libpgtcl jdbc) \
	$(addprefix src/pl/, plperl plpython tcl)

docs_files := doc/postgres.tar.gz doc/src doc/TODO.detail

postgresql-base-$(VERSION).tar: distdir
	$(TAR) -c $(addprefix --exclude $(distdir)/, $(docs_files) $(opt_files) src/test) \
	  -f $@ $(distdir)

postgresql-docs-$(VERSION).tar: distdir
	$(TAR) cf $@ $(addprefix $(distdir)/, $(docs_files))

postgresql-opt-$(VERSION).tar: distdir
	$(TAR) cf $@ $(addprefix $(distdir)/, $(opt_files))

postgresql-test-$(VERSION).tar: distdir
	$(TAR) cf $@ $(distdir)/src/test

distdir:
	-rm -rf $(distdir)* $(dummy)
	for x in `cd $(top_srcdir) && find . -name CVS -prune -o -print`; do \
	  file=`expr X$$x : 'X\./\(.*\)'`; \
	  if test -d "$(top_srcdir)/$$file" ; then \
	    mkdir "$(distdir)/$$file" && chmod 777 "$(distdir)/$$file";	\
	  else \
	    ln "$(top_srcdir)/$$file" "$(distdir)/$$file" >/dev/null 2>&1 \
	      || cp "$(top_srcdir)/$$file" "$(distdir)/$$file"; \
	  fi || exit; \
	done
	$(MAKE) -C $(distdir) distprep
	$(MAKE) -C $(distdir) distclean
	rm -f $(distdir)/README.CVS

distcheck: $(distdir).tar.gz
	-rm -rf $(dummy)
	mkdir $(dummy)
	$(GZIP) -d -c $< | $(TAR) xf -
	install_prefix=`cd $(dummy) && pwd`; \
	cd $(distdir) \
	&& ./configure --prefix="$$install_prefix"
	$(MAKE) -C $(distdir) -q distprep
	$(MAKE) -C $(distdir)
	$(MAKE) -C $(distdir) install
	$(MAKE) -C $(distdir) uninstall
	@echo "checking whether \`$(MAKE) uninstall' works"
	test `find $(dummy) ! -type d | wc -l` -eq 0
	$(MAKE) -C $(distdir) dist
# Room for improvement: Check here whether this distribution tarball
# is sufficiently similar to the original one.
	-rm -rf $(distdir) $(dummy)
	@echo "Distribution integrity checks out."

.PHONY: dist distdir distcheck
unexport split-dist
