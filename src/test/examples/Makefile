#
# Makefile for example programs
#

subdir = src/test/examples
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override LDLIBS := $(libpq) -lpgport $(LDLIBS)


PROGS = testlibpq testlibpq2 testlibpq3 testlibpq4 testlo

all: $(PROGS)

clean: 
	rm -f $(PROGS)
