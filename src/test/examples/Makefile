#
# Makefile for example programs
#

SRCDIR= ../..
LIBPQDIR= $(SRCDIR)/libpq
include ../../Makefile.global

CFLAGS+= -I$(LIBPQDIR)

LD_ADD+= -L$(LIBPQDIR) -lpq 

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LD_ADD+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

PROGS= testlibpq0 testlibpq1 testlibpq2 testlibpq3 testlibpq4 testlo

all: $(PROGS)

$(PROGS): % : %.c 
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $@.c $(LD_ADD)

clean: 
	rm -f $(PROGS)
