# $Header: /cvsroot/pgsql/src/test/locale/Makefile,v 1.5 2000/08/31 16:12:25 petere Exp $

subdir = src/test/locale
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PROGS = test-pgsql-locale test-ctype
DIRS = koi8-r ISO8859-7 koi8-to-win1251

all: $(PROGS)

$(PROGS): % : %.c 
	$(CC) $(CFLAGS) -o $@ $@.c $(LDFLAGS)

clean: 
	rm -f $(PROGS) *.out
	for d in $(DIRS); do \
		$(MAKE) -C $$d clean
	done

test-%: all
	@cd `echo $@ | sed 's/^test-//'` && $(MAKE) test
