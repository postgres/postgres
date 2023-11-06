#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication
#
# IDENTIFICATION
#    src/backend/replication/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/replication
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	repl_gram.o \
	repl_scanner.o \
	slot.o \
	slotfuncs.o \
	syncrep.o \
	syncrep_gram.o \
	syncrep_scanner.o \
	walreceiver.o \
	walreceiverfuncs.o \
	walsender.o

SUBDIRS = logical

include $(top_srcdir)/src/backend/common.mk

# See notes in src/backend/parser/Makefile about the following two rules
repl_gram.h: repl_gram.c
	touch $@

repl_gram.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
repl_gram.o repl_scanner.o: repl_gram.h

# See notes in src/backend/parser/Makefile about the following two rules
syncrep_gram.h: syncrep_gram.c
	touch $@

syncrep_gram.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
syncrep_gram.o syncrep_scanner.o: syncrep_gram.h

clean:
	rm -f repl_gram.c \
	      repl_gram.h \
	      repl_scanner.c \
	      syncrep_gram.c \
	      syncrep_gram.h \
	      syncrep_scanner.c
