# Makefile for erServer demonstration implementation
# (c) 2000 Vadim Mikheev, PostgreSQL Inc.

#vpath %.pl perl
#vpath %.pm perl

subdir = contrib/rserv
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	= rserv
OBJS	= $(NAME).o
DOCS	= README.$(NAME)
SQLS	= master.sql slave.sql
TCLS	= RservTest
PERLS	= MasterInit SlaveInit MasterAddTable SlaveAddTable Replicate CleanLog
PERLS	+= PrepareSnapshot ApplySnapshot GetSyncID SyncSyncID
LIBS	= RServ.pm
SCRIPTS	= InitRservTest
MODS	= $(OBJS:.o=$(DLSUFFIX))

override CPPFLAGS += -I$(srcdir)
override CFLAGS += $(CFLAGS_SL)

INPUTFILES = $(wildcard *.in)
CLEANFILES = $(INPUTFILES:.in=)
CLEANFILES += $(OBJS) $(MODS)

.PHONY: all install installdirs tarball

all: $(SQLS) $(TCLS) $(PERLS) $(SCRIPTS) $(MODS)

install: all installdirs
	$(INSTALL_DATA) $(SQLS) $(libdir)/contrib
	$(INSTALL_SCRIPT) $(TCLS) $(PERLS) $(SCRIPTS) $(bindir)
	$(INSTALL_SCRIPT) $(LIBS) $(libdir)/contrib
	$(INSTALL_SHLIB) $(MODS) $(libdir)/contrib
	$(INSTALL_DATA) $(DOCS) $(docdir)/contrib/$(NAME)

installdirs:
	$(mkinstalldirs) $(datadir)/contrib $(libdir)/contrib $(docdir)/contrib/$(NAME)

%.sql: %.sql.in
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$(libdir)/contrib:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

%: %.in
	sed -e "s:_OBJWD_:$(libdir)/contrib:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s:@SQLDIR@:$(libdir)/contrib:g" \
	    -e "s:@BINDIR@:$(bindir):g" \
	    -e "s:@LIBDIR@:$(libdir)/contrib:g" < $< > $@
	chmod 775 $@

clean:
#	@echo "Removing $(CLEANFILES)"
	rm -f $(CLEANFILES)
