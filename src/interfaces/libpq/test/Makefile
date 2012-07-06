subdir = src/interfaces/libpq/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDLIBS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDLIBS := $(libpq_pgport) $(LDLIBS)

PROGS = uri-regress

all: $(PROGS)

installcheck: all
	SRCDIR='$(top_srcdir)' SUBDIR='$(subdir)' \
		   $(PERL) $(top_srcdir)/$(subdir)/regress.pl

clean distclean maintainer-clean:
	rm -f $(PROGS)
	rm -f regress.out regress.diff
