# Makefile for erServer demonstration implementation
# (c) 2000 Vadim Mikheev, PostgreSQL Inc.

subdir = contrib/rserv
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	= rserv
SONAME	= $(NAME)$(DLSUFFIX)
SQLS	= master.sql slave.sql
TCLS	= RservTest
PERLS	= MasterInit MasterAddTable Replicate MasterSync CleanLog
PERLS	+= SlaveInit SlaveAddTable GetSyncID
PERLS	+= PrepareSnapshot ApplySnapshot
SCRIPTS	= InitRservTest

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)
override DLLLIBS := $(BE_DLLLIBS) $(DLLLIBS)


all: $(SQLS) $(TCLS) $(PERLS) $(SCRIPTS) $(SONAME)

%.sql: %.sql.in
	sed 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' $< >$@

$(PERLS) $(TCLS) $(SCRIPTS): %: %.in
	sed -e 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' \
	    -e 's:@SQLDIR@:$(datadir)/contrib:g' \
	    -e 's:@BINDIR@:$(bindir):g' \
	    -e 's:@LIBDIR@:$(datadir)/contrib:g' $< >$@
	chmod a+x $@


install: all installdirs
	for file in $(SQLS); do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(datadir)/contrib || exit ; \
	done
	for file in $(TCLS) $(PERLS) $(SCRIPTS); do \
	  $(INSTALL_SCRIPT) $$file $(DESTDIR)$(bindir) || exit ; \
	done
	$(INSTALL_DATA) $(srcdir)/RServ.pm $(DESTDIR)$(datadir)/contrib
	$(INSTALL_SHLIB) $(SONAME) $(DESTDIR)$(pkglibdir)
	$(INSTALL_DATA) $(srcdir)/README.$(NAME) $(DESTDIR)$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)/contrib \
	  $(DESTDIR)$(pkglibdir) $(DESTDIR)$(docdir)/contrib


clean distclean maintainer-clean:
	rm -f $(SQLS) $(TCLS) $(PERLS) $(SCRIPTS) $(SONAME) $(NAME).o
