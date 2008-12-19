#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for foreign
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/foreign/Makefile,v 1.1 2008/12/19 16:25:17 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/foreign
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= foreign.o

include $(top_srcdir)/src/backend/common.mk

FDW = dummy postgresql

$(addsuffix -fdw,all install installdirs uninstall distprep):
	for dir in $(FDW); do $(MAKE) -C $$dir `echo $@ | sed 's/-fdw$$//'` || exit; done

clean distclean maintainer-clean:
	for dir in $(FDW); do $(MAKE) -C $$dir $@ || exit; done
