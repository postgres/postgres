#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/Makefile,v 1.15 2000/11/30 20:36:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = common.o conv.o mbutils.o wchar.o wstrcmp.o wstrncmp.o big5.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

utftest.o: utftest.c conv.c wchar.c mbutils.c

sjistest: sjistest.o palloc.o common.o mbutils.o wchar.o wstrcmp.o wstrncmp.o big5.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

liketest: liketest.o palloc.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

utftest: utftest.o palloc.o common.o wstrcmp.o wstrncmp.o big5.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
