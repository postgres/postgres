#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regex
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/regex/Makefile,v 1.19 2002/09/16 16:02:43 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/regex
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -DPOSIX_MISTAKE 

OBJS = regcomp.o regerror.o regexec.o regfree.o
DEBUGOBJ += ../utils/mb/SUBSYS.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

regexec.o: regexec.c engine.c

# retest will not compile because multibyte is now enabled by default
# and the multibyte calls require /mmgr, /adt, and other calls that
# are complex for linkage,  bjm 2002-09-16
#retest: retest.o SUBSYS.o $(DEBUGOBJ)
#	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

clean: 
	rm -f SUBSYS.o $(OBJS) retest retest.o
