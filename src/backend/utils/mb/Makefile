#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/Makefile,v 1.11 2000/10/12 06:06:49 ishii Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = common.o conv.o mbutils.o wchar.o wstrcmp.o wstrncmp.o variable.o \
       big5.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

palloc.o: palloc.c
	$(CC) -c $(CFLAGS) palloc.c

sjistest.o: sjistest.c
	$(CC) -c $(CFLAGS) sjistest.c

liketest.o: liketest.c
	$(CC) -c $(CFLAGS) liketest.c

uconv.o: uconv.c
	$(CC) -c $(CFLAGS) uconv.c

uconv2.o: uconv2.c
	$(CC) -c $(CFLAGS) uconv2.c

utftest.o: utftest.c conv.c wchar.c mbutils.c
	$(CC) -c $(CFLAGS) utftest.c

sjistest: $(OBJS) sjistest.o palloc.o
	$(CC) -o sjistest sjistest.o palloc.o \
	common.o mbutils.o wchar.o wstrcmp.o wstrncmp.o variable.o \
       big5.o $(LDFLAGS)

liketest: $(OBJS) liketest.o palloc.o
	$(CC) -o liketest liketest.o palloc.o conv.o \
	common.o mbutils.o wchar.o wstrcmp.o wstrncmp.o variable.o \
       big5.o $(LDFLAGS)

utftest: $(OBJS) utftest.o palloc.o
	$(CC) -o utftest utftest.o palloc.o \
	common.o wstrcmp.o wstrncmp.o variable.o \
       big5.o $(LDFLAGS)

uconv: uconv.o palloc.o
	$(CC) -o uconv uconv.o palloc.o \
		common.o conv.o wchar.o \
       big5.o mbutils.o $(LDFLAGS)

uconv2: uconv2.o palloc.o
	$(CC) -o uconv2 uconv2.o palloc.o \
		common.o conv.o wchar.o \
       big5.o mbutils.o $(LDFLAGS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
