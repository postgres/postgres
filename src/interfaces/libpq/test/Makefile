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

installcheck: all
	SRCDIR='$(top_srcdir)' SUBDIR='$(subdir)' \
		   $(PERL) $(top_srcdir)/$(subdir)/regress.pl

clean distclean maintainer-clean:
	rm -f $(PROGS) *.o
	rm -f regress.out regress.diff
