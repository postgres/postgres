#-------------------------------------------------------------------------
#
# Makefile for the bootstrap module
#
# src/backend/bootstrap/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/bootstrap
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	bootparse.o \
	bootscanner.o \
	bootstrap.o

include $(top_srcdir)/src/backend/common.mk

# See notes in src/backend/parser/Makefile about the following two rules
bootparse.h: bootparse.c
	touch $@

bootparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
bootparse.o bootscanner.o: bootparse.h

clean:
	rm -f bootparse.c \
	      bootparse.h \
	      bootscanner.c
