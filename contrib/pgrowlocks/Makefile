#-------------------------------------------------------------------------
#
# pgrowlocks Makefile
#
# $PostgreSQL: pgsql/contrib/pgrowlocks/Makefile,v 1.1 2006/04/23 01:12:58 ishii Exp $
#
#-------------------------------------------------------------------------

SRCS		= pgrowlocks.c

MODULE_big	= pgrowlocks
OBJS		= $(SRCS:.c=.o)
DOCS		= README.pgrowlocks README.pgrowlocks.euc_jp
DATA_built	= pgrowlocks.sql

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgrowlocks
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

