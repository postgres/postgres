subdir = src/interfaces/ecpg
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all install installdirs uninstall distprep:
	$(MAKE) -C include $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C ecpglib $@
	$(MAKE) -C compatlib $@
	$(MAKE) -C preproc $@

clean distclean maintainer-clean:
	-$(MAKE) -C include $@
	-$(MAKE) -C pgtypeslib $@
	-$(MAKE) -C ecpglib $@
	-$(MAKE) -C compatlib $@
	-$(MAKE) -C preproc $@
	-$(MAKE) -C test clean

check checktcp installcheck: all
	$(MAKE) -C test $@
