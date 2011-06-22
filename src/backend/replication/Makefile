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

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS = walsender.o walreceiverfuncs.o walreceiver.o basebackup.o \
	repl_gram.o syncrep.o

include $(top_srcdir)/src/backend/common.mk

# repl_scanner is compiled as part of repl_gram
repl_gram.o: repl_scanner.c

# See notes in src/backend/parser/Makefile about the following two rules

repl_gram.c: repl_gram.y
ifdef BISON
	$(BISON) -d $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

repl_scanner.c: repl_scanner.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

# repl_gram.c and repl_scanner.c are in the distribution tarball, so
# they are not cleaned here.
