#
# Makefile for example programs
#

subdir = src/test/examples
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LIBS += $(libpq)


# PROGS= testlibpq0 testlibpq1 testlibpq2 testlibpq3 testlibpq4 testlo
PROGS = testlibpq testlibpq2 testlibpq3 testlibpq4 testlo

all: $(PROGS)

clean: 
	rm -f $(PROGS)
