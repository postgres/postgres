subdir = src/interfaces/ecpg
top_builddir = ../../..
include ../../Makefile.global

all install installdirs uninstall dep depend:
	$(MAKE) -C include $@
	$(MAKE) -C lib $@
	$(MAKE) -C preproc $@

clean distclean maintainer-clean:
	-$(MAKE) -C include $@
	-$(MAKE) -C lib $@
	-$(MAKE) -C preproc $@
