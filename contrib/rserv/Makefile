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
SCRIPTS_built	= RservTest
SCRIPTS_built	+= MasterInit MasterAddTable Replicate MasterSync CleanLog
SCRIPTS_built	+= SlaveInit SlaveAddTable GetSyncID
SCRIPTS_built	+= PrepareSnapshot ApplySnapshot
SCRIPTS_built	+= InitRservTest


$(SQLS): %.sql: %.sql.in
	sed 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' $< >$@

$(SCRIPTS_built): %: %.in
	sed -e 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' \
	    -e 's:@SQLDIR@:$(datadir)/contrib:g' \
	    -e 's:@BINDIR@:$(bindir):g' \
	    -e 's:@LIBDIR@:$(datadir)/contrib:g' $< >$@
	chmod a+x $@

include $(top_srcdir)/contrib/contrib-global.mk
