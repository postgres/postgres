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

override CPPFLAGS += -I$(srcdir)
override CFLAGS += $(CFLAGS_SL)


all: $(SQLS) $(TCLS) $(PERLS) $(SCRIPTS) $(SONAME)

%.sql: %.sql.in
	sed -e "s:_OBJWD_:$(libdir)/contrib:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" $< >$@

$(PERLS) $(TCLS) $(SCRIPTS): %: %.in
	sed -e "s:_OBJWD_:$(libdir)/contrib:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s:@SQLDIR@:$(datadir)/contrib:g" \
	    -e "s:@BINDIR@:$(bindir):g" \
	    -e "s:@LIBDIR@:$(datadir)/contrib:g" $< >$@
	chmod a+x $@


install: all installdirs
	for file in $(SQLS); do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(datadir)/contrib || exit ; \
	done
	for file in $(TCLS) $(PERLS) $(SCRIPTS); do \
	  $(INSTALL_SCRIPT) $$file $(DESTDIR)$(bindir) || exit ; \
	done
	$(INSTALL_DATA) RServ.pm $(DESTDIR)$(datadir)/contrib
	$(INSTALL_SHLIB) $(SONAME) $(DESTDIR)$(libdir)/contrib
	$(INSTALL_DATA) README.$(NAME) $(DESTDIR)$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)/contrib \
	  $(DESTDIR)$(libdir)/contrib $(DESTDIR)$(docdir)/contrib


clean distclean maintainer-clean:
	rm -f $(SQLS) $(TCLS) $(PERLS) $(SCRIPTS) $(SONAME) $(NAME).o
