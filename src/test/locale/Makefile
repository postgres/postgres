#
# Makefile for example programs
#

SRCDIR= ../..
include ../../Makefile.global

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

PROGS = test-pgsql-locale test-ctype
DIRS = koi8-r ISO8859-7 koi8-to-win1251

all: $(PROGS)

$(PROGS): % : %.c 
	$(CC) $(CFLAGS) -o $@ $@.c $(LDFLAGS)

clean: 
	rm -f $(PROGS) *.out
	for d in $(DIRS); do \
		cd $$d; \
		$(MAKE) clean; \
		cd ..; \
	done

test-%: all
	@cd `echo $@ | sed 's/^test-//'` && $(MAKE) test
