# src/interfaces/libpq/test/Makefile

subdir = src/interfaces/libpq/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDFLAGS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)

PROGS = uri-regress

all: $(PROGS)

clean distclean maintainer-clean:
	rm -f $(PROGS) *.o
