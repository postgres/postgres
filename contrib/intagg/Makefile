#############################################
# Makefile for integer aggregator
# Copyright (C) 2001 Digital Music Network.
# by Mark L. Woodward
#
subdir = contrib/intagg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME=int_aggregate
SONAME	= $(NAME)$(DLSUFFIX)
MODULES = int_aggregate
DATA_built = int_aggregate.so
DOCS = README.int_aggregate
SQLS=int_aggregate.sql

include $(top_srcdir)/contrib/contrib-global.mk

%.sql: %.sql.in
	sed 's,MODULE_FILENAME,$$libdir/$(NAME),g' $< >$@

all : $(SONAME) $(SQLS)


install : all
	$(INSTALL_SHLIB) $(SONAME) $(DESTDIR)$(pkglibdir)
		

clean :
	rm -f $(SONAME)
	rm -f $(SQLS)
