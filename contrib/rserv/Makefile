# Makefile for erServer demonstration implementation
# (c) 2000 Vadim Mikheev, PostgreSQL Inc.

subdir = contrib/rserv
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	= rserv
MODULES	= rserv
DATA	= RServ.pm
DATA_built	= master.sql slave.sql
SQLS	= $(DATA_built)
DOCS	= README.rserv
SCRIPTS	= RservTest
SCRIPTS	+= MasterInit MasterAddTable Replicate MasterSync CleanLog
SCRIPTS	+= SlaveInit SlaveAddTable GetSyncID
SCRIPTS	+= PrepareSnapshot ApplySnapshot
SCRIPTS	+= InitRservTest
EXTRA_CLEAN	= $(SCRIPTS)

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)
SHLIB_LINK = $(BE_DLLLIBS)


all: $(SCRIPTS) $(SQLS) $(addsuffix $(DLSUFFIX), $(MODULES))

$(SQLS): %.sql: %.sql.in
	sed 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' $< >$@

$(SCRIPTS): %: %.in
	sed -e 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' \
	    -e 's:@SQLDIR@:$(datadir)/contrib:g' \
	    -e 's:@BINDIR@:$(bindir):g' \
	    -e 's:@LIBDIR@:$(datadir)/contrib:g' $< >$@
	chmod a+x $@

include $(top_srcdir)/contrib/contrib-global.mk
