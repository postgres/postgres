#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/libpq/Makefile,v 1.26 2001/11/12 01:42:03 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = be-fsstubs.o \
	auth.o crypt.o hba.o md5.o password.o \
	pqcomm.o pqformat.o pqpacket.o pqsignal.o util.o


all: check_md5 SUBSYS.o

check_md5:
	@cmp -s md5.c ../../interfaces/odbc/md5.c || \
	(echo "src/interfaces/odbc/md5.c doesn't match src/backend/libpq/md5.c" && \
	 exit 1)

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
