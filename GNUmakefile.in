#
# PostgreSQL top level makefile
#
# GNUmakefile.in
#

subdir =
top_builddir = .
include $(top_builddir)/src/Makefile.global

$(call recurse,all install,src config)

docs:
	$(MAKE) -C doc all

$(call recurse,world,doc src config contrib,all)

# build src/ before contrib/
world-contrib-recurse: world-src-recurse

$(call recurse,world-bin,src config contrib,all)

# build src/ before contrib/
world-bin-contrib-recurse: world-bin-src-recurse

html man:
	$(MAKE) -C doc $@

install-docs:
	$(MAKE) -C doc install

$(call recurse,install-world,doc src config contrib,install)

# build src/ before contrib/
install-world-contrib-recurse: install-world-src-recurse

$(call recurse,install-world-bin,src config contrib,install)

# build src/ before contrib/
install-world-bin-contrib-recurse: install-world-bin-src-recurse

$(call recurse,installdirs uninstall init-po update-po,doc src config)

$(call recurse,coverage,doc src config contrib)

# clean, distclean, etc should apply to contrib too, even though
# it's not built by default
$(call recurse,clean,doc contrib src config)
clean:
	rm -rf tmp_install/ portlock/
# Garbage from autoconf:
	@rm -rf autom4te.cache/

# Important: distclean `src' last, otherwise Makefile.global
# will be gone too soon.
distclean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C config $@
	$(MAKE) -C src $@
	rm -rf tmp_install/ portlock/
# Garbage from autoconf:
	@rm -rf autom4te.cache/
	rm -f config.cache config.log config.status GNUmakefile

check-tests: | temp-install
check check-tests installcheck installcheck-parallel installcheck-tests: CHECKPREP_TOP=src/test/regress
check check-tests installcheck installcheck-parallel installcheck-tests: submake-generated-headers
	$(MAKE) -C src/test/regress $@

$(call recurse,check-world,src/test src/pl src/interfaces contrib src/bin src/tools/pg_bsd_indent,check)
$(call recurse,checkprep,  src/test src/pl src/interfaces contrib src/bin)

$(call recurse,installcheck-world,src/test src/pl src/interfaces contrib src/bin,installcheck)
$(call recurse,install-tests,src/test/regress,install-tests)

GNUmakefile: GNUmakefile.in $(top_builddir)/config.status
	./config.status $@

update-unicode: | submake-generated-headers submake-libpgport
	$(MAKE) -C src/common/unicode $@
	$(MAKE) -C contrib/unaccent $@


##########################################################################

distdir	= postgresql-$(VERSION)
dummy	= =install=

# git revision to be packaged
PG_GIT_REVISION = HEAD

GIT = git

dist: $(distdir).tar.gz $(distdir).tar.bz2

.PHONY: $(distdir).tar.gz $(distdir).tar.bz2

distdir-location:
	@echo $(distdir)

# Note: core.autocrlf=false is needed to avoid line-ending conversion
# in case the environment has a different setting.  Without this, a
# tarball created on Windows might be different than on, and unusable
# on, Unix machines.

$(distdir).tar.gz:
	$(GIT) -C $(srcdir) -c core.autocrlf=false archive --format tar.gz -9 --prefix $(distdir)/ $(PG_GIT_REVISION) -o $(abs_top_builddir)/$@

$(distdir).tar.bz2:
	$(GIT) -C $(srcdir) -c core.autocrlf=false -c tar.tar.bz2.command='$(BZIP2) -c' archive --format tar.bz2 --prefix $(distdir)/ $(PG_GIT_REVISION) -o $(abs_top_builddir)/$@

distcheck: dist
	rm -rf $(dummy)
	mkdir $(dummy)
	$(GZIP) -d -c $(distdir).tar.gz | $(TAR) xf -
	install_prefix=`cd $(dummy) && pwd`; \
	cd $(distdir) \
	&& ./configure --prefix="$$install_prefix"
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
	$(top_srcdir)/src/tools/pginclude/headerscheck --cplusplus $(top_srcdir) $(abs_top_builddir)

.PHONY: dist distcheck docs install-docs world check-world install-world installcheck-world headerscheck cpluspluscheck
