#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regex
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/regex/Makefile,v 1.16 2000/11/30 20:36:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/regex
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -DPOSIX_MISTAKE 

DEBUGOBJ =

OBJS = regcomp.o regerror.o regexec.o regfree.o

ifdef MULTIBYTE
DEBUGOBJ += ../utils/mb/SUBSYS.o
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

retest: retest.o SUBSYS.o $(DEBUGOBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) retest retest.o

ifeq (depend,$(wildcard depend))
include depend
endif
