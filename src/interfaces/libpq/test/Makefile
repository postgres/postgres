# src/interfaces/libpq/test/Makefile

PGFILEDESC = "libpq test program"
PGAPPICON = win32

subdir = src/interfaces/libpq/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDFLAGS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)

PROGS = libpq_testclient libpq_uri_regress

all: $(PROGS)

$(PROGS): $(WIN32RES)

clean distclean:
	rm -f $(PROGS) *.o
