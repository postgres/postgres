#-------------------------------------------------------------------------
#
# Makefile for src/test/perl
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/perl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/perl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(enable_tap_tests),yes)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

install: all installdirs
	$(INSTALL_DATA) $(srcdir)/TestLib.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/TestLib.pm'
	$(INSTALL_DATA) $(srcdir)/SimpleTee.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/SimpleTee.pm'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/TestLib.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/SimpleTee.pm'

endif
