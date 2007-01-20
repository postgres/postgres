#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/lmgr/Makefile,v 1.22 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/lmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = lmgr.o lock.o proc.o deadlock.o lwlock.o spin.o s_lock.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

ifdef TAS
TASPATH = $(top_builddir)/src/backend/port/tas.o
endif

s_lock_test: s_lock.c $(top_builddir)/src/port/libpgport.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -DS_LOCK_TEST=1 $(srcdir)/s_lock.c \
		$(TASPATH) -L $(top_builddir)/src/port -lpgport -o s_lock_test

check: s_lock_test
	./s_lock_test

clean distclean maintainer-clean: 
	rm -f SUBSYS.o $(OBJS) s_lock_test
