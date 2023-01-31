#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for statistics
#
# IDENTIFICATION
#    src/backend/statistics/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/statistics
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	dependencies.o \
	extended_stats.o \
	mcv.o \
	mvdistinct.o \
	statistics_gram.o \
	statistics_scanner.o

include $(top_srcdir)/src/backend/common.mk

# See notes in src/backend/parser/Makefile about the following two rules
statistics_gram.h: statistics_gram.c
	touch $@

statistics_gram.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
statistics_gram.o statistics_scanner.o: statistics_gram.h

# statistics_gram.c, statistics_scanner are not cleaned here.
# (Our parent Makefile takes care of them during maintainer-clean.)
