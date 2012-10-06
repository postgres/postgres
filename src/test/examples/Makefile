#
# Makefile for example programs
#

subdir = src/test/examples
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDLIBS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDLIBS := $(libpq_pgport) $(LDLIBS)


PROGS = testlibpq testlibpq2 testlibpq3 testlibpq4 testlo testlo64

all: $(PROGS)

clean:
	rm -f $(PROGS)
