# $Header: /cvsroot/pgsql/src/test/locale/Makefile,v 1.6 2000/10/20 21:04:24 petere Exp $

subdir = src/test/locale
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PROGS = test-pgsql-locale test-ctype
DIRS = koi8-r ISO8859-7 koi8-to-win1251

all: $(PROGS)

clean: 
	rm -f $(PROGS) *.out
	for d in $(DIRS); do \
		$(MAKE) -C $$d clean
	done

test-%: all
	@cd `echo $@ | sed 's/^test-//'` && $(MAKE) test
