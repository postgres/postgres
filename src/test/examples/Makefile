#
# Makefile for example programs
#

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+= -I$(LIBPQDIR)

LDFLAGS+= -L$(LIBPQDIR) -lpq 


# PROGS= testlibpq0 testlibpq1 testlibpq2 testlibpq3 testlibpq4 testlo
PROGS = testlibpq testlibpq2 testlibpq3 testlibpq4 testlo testlo2

all: $(PROGS)

$(PROGS): % : %.c 
	$(CC) $(CFLAGS) -o $@ $@.c $(LDFLAGS)

clean: 
	rm -f $(PROGS)
