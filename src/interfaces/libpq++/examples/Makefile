#
# Makefile for example programs
#


LIBNAME= libpq++
HEADERDIR= /usr/local/pgsql/include
LIBPQDIR= /usr/local/pgsql/lib


# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

#CXXFLAGS= $(CFLAGS) -Wno-error -Wno-unused -Wl,-Bdynamic
CXXFLAGS= $(CFLAGS)

INCLUDE_OPT= -I$(HEADERDIR)

CXXFLAGS+= $(INCLUDE_OPT)

LDFLAGS+= -L$(LIBPQDIR) -lpq++ 

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CXXFLAGS+= $(KRBFLAGS)
endif

PROGS=	testlibpq0 testlibpq1 testlibpq2 testlibpq3 \
	testlibpq4 testlibpq5 testlibpq6 testlo

all: $(PROGS)

$(PROGS): % : %.cc
	$(CXX) $(CXXFLAGS) -o $@ $@.cc $(LDFLAGS)

.PHONY: submake

clean: 
	rm -f $(PROGS)

