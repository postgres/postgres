#
# Makefile for example programs
#

SRCDIR= ../..
include ../../Makefile.global

# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

CXXFLAGS= $(CFLAGS) -Wno-error

INCLUDE_OPT= \
             -I.. \
             -I../../backend \
             -I../../include \
             -I$(LIBPQDIR) 

CXXFLAGS+= $(INCLUDE_OPT)

LD_ADD+= -L.. -lpq++ -L$(LIBPQDIR) -lpq 

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LD_ADD+= $(KRBLIBS)
CXXFLAGS+= $(KRBFLAGS)
endif

PROGS= testlibpq0 testlibpq1 testlibpq2 testlibpq3 testlibpq4 testlo

all: submake $(PROGS)

$(PROGS): % : %.cc ../libpq++.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $@.cc $(LD_ADD)

.PHONY: submake
submake: 
	$(MAKE) -C.. libpq++.a

../libpq++.a:
	$(MAKE) -C.. libpq++.a

clean: 
	rm -f $(PROGS)
