#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    src/backend/storage/lmgr/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/lmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	condition_variable.o \
	deadlock.o \
	lmgr.o \
	lock.o \
	lwlock.o \
	predicate.o \
	proc.o \
	s_lock.o \

include $(top_srcdir)/src/backend/common.mk

ifdef TAS
TASPATH = $(top_builddir)/src/backend/port/tas.o
endif

s_lock_test: s_lock.c $(top_builddir)/src/common/libpgcommon.a $(top_builddir)/src/port/libpgport.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -DS_LOCK_TEST=1 $(srcdir)/s_lock.c \
		$(TASPATH) -L $(top_builddir)/src/common -lpgcommon \
		-L $(top_builddir)/src/port -lpgport -lm -o s_lock_test

lwlocknames.h: ../../../include/storage/lwlocklist.h ../../utils/activity/wait_event_names.txt generate-lwlocknames.pl
	$(PERL) $(srcdir)/generate-lwlocknames.pl $^

check: s_lock_test
	./s_lock_test

clean:
	rm -f s_lock_test
	rm -f lwlocknames.h
