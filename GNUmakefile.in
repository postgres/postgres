#
# PostgreSQL top level makefile
#
# GNUmakefile.in
#

subdir =
top_builddir = .
include $(top_builddir)/src/Makefile.global

$(call recurse,all install,src config)

all:
	+@echo "All of PostgreSQL successfully made. Ready to install."

docs:
	$(MAKE) -C doc all

$(call recurse,world,doc src config contrib,all)
world:
	+@echo "PostgreSQL, contrib, and documentation successfully made. Ready to install."

# build src/ before contrib/
world-contrib-recurse: world-src-recurse

html man:
	$(MAKE) -C doc $@

install:
	+@echo "PostgreSQL installation complete."

install-docs:
	$(MAKE) -C doc install

$(call recurse,install-world,doc src config contrib,install)
install-world:
	+@echo "PostgreSQL, contrib, and documentation installation complete."

# build src/ before contrib/
install-world-contrib-recurse: install-world-src-recurse

$(call recurse,installdirs uninstall init-po update-po,doc src config)

$(call recurse,distprep coverage,doc src config contrib)

# clean, distclean, etc should apply to contrib too, even though
# it's not built by default
$(call recurse,clean,doc contrib src config)
clean:
	rm -rf tmp_install/
# Garbage from autoconf:
	@rm -rf autom4te.cache/

# Important: distclean `src' last, otherwise Makefile.global
# will be gone too soon.
distclean maintainer-clean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C config $@
	$(MAKE) -C src $@
	rm -rf tmp_install/
# Garbage from autoconf:
	@rm -rf autom4te.cache/
	rm -f config.cache config.log config.status GNUmakefile

check-tests: | temp-install
check check-tests installcheck installcheck-parallel installcheck-tests: CHECKPREP_TOP=src/test/regress
check check-tests installcheck installcheck-parallel installcheck-tests: submake-generated-headers
	$(MAKE) -C src/test/regress $@

$(call recurse,check-world,src/test src/pl src/interfaces/ecpg contrib src/bin,check)
$(call recurse,checkprep,  src/test src/pl src/interfaces/ecpg contrib src/bin)

$(call recurse,installcheck-world,src/test src/pl src/interfaces/ecpg contrib src/bin,installcheck)
$(call recurse,install-tests,src/test/regress,install-tests)

GNUmakefile: GNUmakefile.in $(top_builddir)/config.status
	./config.status $@

update-unicode: | submake-generated-headers submake-libpgport
	$(MAKE) -C src/common/unicode $@
	$(MAKE) -C contrib/unaccent $@


##########################################################################

distdir	= postgresql-$(VERSION)
dummy	= =install=

dist: $(distdir).tar.gz $(distdir).tar.bz2
	rm -rf $(distdir)

$(distdir).tar: distdir
	$(TAR) chf $@ $(distdir)

.INTERMEDIATE: $(distdir).tar

distdir-location:
	@echo $(distdir)

distdir:
	rm -rf $(distdir)* $(dummy)
	for x in `cd $(top_srcdir) && find . \( -name CVS -prune \) -o \( -name .git -prune \) -o -print`; do \
	  file=`expr X$$x : 'X\./\(.*\)'`; \
	  if test -d "$(top_srcdir)/$$file" ; then \
	    mkdir "$(distdir)/$$file" && chmod 777 "$(distdir)/$$file";	\
	  else \
	    ln "$(top_srcdir)/$$file" "$(distdir)/$$file" >/dev/null 2>&1 \
	      || cp "$(top_srcdir)/$$file" "$(distdir)/$$file"; \
	  fi || exit; \
	done
	$(MAKE) -C $(distdir) distprep
	$(MAKE) -C $(distdir)/doc/src/sgml/ INSTALL
	cp $(distdir)/doc/src/sgml/INSTALL $(distdir)/
	$(MAKE) -C $(distdir) distclean
	rm -f $(distdir)/README.git

distcheck: dist
	rm -rf $(dummy)
	mkdir $(dummy)
	$(GZIP) -d -c $(distdir).tar.gz | $(TAR) xf -
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
	rm -rf $(distdir) $(dummy)
	@echo "Distribution integrity checks out."

headerscheck: submake-generated-headers
	$(top_srcdir)/src/tools/pginclude/headerscheck $(top_srcdir) $(abs_top_builddir)

cpluspluscheck: submake-generated-headers
	$(top_srcdir)/src/tools/pginclude/cpluspluscheck $(top_srcdir) $(abs_top_builddir)

.PHONY: dist distdir distcheck docs install-docs world check-world install-world installcheck-world headerscheck cpluspluscheck
