#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/Makefile,v 1.13 2000/10/25 19:44:44 tgl Exp $
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
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

liketest: liketest.o palloc.o $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

utftest: utftest.o palloc.o common.o wstrcmp.o wstrncmp.o big5.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

uconv: uconv.o palloc.o common.o conv.o wchar.o big5.o mbutils.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

uconv2: uconv2.o palloc.o common.o conv.o wchar.o big5.o mbutils.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
