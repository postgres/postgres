#
# Makefile for example programs
#

SRCDIR= ../..
include ../../Makefile.global

LIBNAME= libpq++

# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

CXXFLAGS= $(CFLAGS) -Wno-error

INCLUDE_OPT= \
             -I.. \
             -I../../backend \
             -I../../include \
             -I$(LIBPQDIR) \
             -I$(HEADERDIR)
CXXFLAGS+= $(INCLUDE_OPT)

LDFLAGS+= -L.. -lpq++ -L$(LIBPQDIR) -lpq 

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CXXFLAGS+= $(KRBFLAGS)
endif

PROGS=	testlibpq0 testlibpq1 testlibpq2 testlibpq3 \
	testlibpq4 testlibpq5 testlibpq6 testlo

all: submake $(PROGS)

$(PROGS): % : %.cc ../$(LIBNAME).a
	$(CXX) $(CXXFLAGS) -o $@ $@.cc $(LDFLAGS)

.PHONY: submake
submake: 
	$(MAKE) -C.. $(LIBNAME).a

../$(LIBNAME).a:
	$(MAKE) -C.. $(LIBNAME).a

clean: 
	rm -f $(PROGS)

