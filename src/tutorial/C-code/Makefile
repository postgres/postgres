SRCDIR= ../..
include ../../Makefile.global

CFLAGS+= -I../../include -I$(LIBPQDIR)

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

all: complex$(DLSUFFIX) funcs$(DLSUFFIX)

clean: 
	rm -f complex$(DLSUFFIX) funcs$(DLSUFFIX)

