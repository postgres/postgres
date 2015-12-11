# config/Makefile

subdir = config
top_builddir = ..
include $(top_builddir)/src/Makefile.global


install: all installdirs
	$(INSTALL_SCRIPT) $(srcdir)/install-sh '$(DESTDIR)$(pgxsdir)/config/install-sh'
	$(INSTALL_SCRIPT) $(srcdir)/missing '$(DESTDIR)$(pgxsdir)/config/missing'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/config'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/config/install-sh'
	rm -f '$(DESTDIR)$(pgxsdir)/config/missing'
