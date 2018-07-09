#
# Makefile for example programs
#

subdir = src/test/examples
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDFLAGS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)


PROGS = testlibpq testlibpq2 testlibpq3 testlibpq4 testlo testlo64

all: $(PROGS)

clean distclean maintainer-clean:
	rm -f $(PROGS) *.o
