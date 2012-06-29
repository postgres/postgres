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

$(call recurse,installdirs uninstall coverage init-po update-po,doc src config)

$(call recurse,distprep,doc src config contrib)

# clean, distclean, etc should apply to contrib too, even though
# it's not built by default
$(call recurse,clean,doc contrib src config)
clean:
# Garbage from autoconf:
	@rm -rf autom4te.cache/

# Important: distclean `src' last, otherwise Makefile.global
# will be gone too soon.
distclean maintainer-clean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C config $@
	$(MAKE) -C src $@
	rm -f config.cache config.log config.status GNUmakefile
# Garbage from autoconf:
	@rm -rf autom4te.cache/

check: all

check installcheck installcheck-parallel:
	$(MAKE) -C src/test/regress $@

$(call recurse,check-world,src/test src/pl src/interfaces/ecpg contrib,check)

$(call recurse,installcheck-world,src/test src/pl src/interfaces/ecpg contrib,installcheck)

$(call recurse,maintainer-check,doc src config contrib)

GNUmakefile: GNUmakefile.in $(top_builddir)/config.status
	./config.status $@


##########################################################################

distdir	= postgresql-$(VERSION)
dummy	= =install=
garbage = =*  "#"*  ."#"*  *~*  *.orig  *.rej  core  postgresql-*

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
	$(MAKE) -C $(distdir)/doc/src/sgml/ HISTORY INSTALL regress_README
	cp $(distdir)/doc/src/sgml/HISTORY $(distdir)/
	cp $(distdir)/doc/src/sgml/INSTALL $(distdir)/
	cp $(distdir)/doc/src/sgml/regress_README $(distdir)/src/test/regress/README
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

.PHONY: dist distdir distcheck docs install-docs world check-world install-world installcheck-world
