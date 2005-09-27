# $PostgreSQL: pgsql/contrib/dbmirror/Makefile,v 1.5 2005/09/27 17:13:01 tgl Exp $

MODULES = pending
SCRIPTS = clean_pending.pl DBMirror.pl
DATA = AddTrigger.sql MirrorSetup.sql slaveDatabase.conf
DOCS = README.dbmirror

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/dbmirror
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
