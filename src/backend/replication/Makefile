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

OBJS = walsender.o walreceiverfuncs.o walreceiver.o basebackup.o \
	repl_gram.o slot.o slotfuncs.o syncrep.o syncrep_gram.o

SUBDIRS = logical

include $(top_srcdir)/src/backend/common.mk

# repl_scanner is compiled as part of repl_gram
repl_gram.o: repl_scanner.c

# syncrep_scanner is complied as part of syncrep_gram
syncrep_gram.o: syncrep_scanner.c
syncrep_scanner.c: FLEXFLAGS = -CF -p
syncrep_scanner.c: FLEX_NO_BACKUP=yes

# repl_gram.c, repl_scanner.c, syncrep_gram.c and syncrep_scanner.c
# are in the distribution tarball, so they are not cleaned here.
