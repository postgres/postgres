# $PostgreSQL: pgsql/src/test/locale/Makefile,v 1.9 2003/11/29 19:52:14 pgsql Exp $

subdir = src/test/locale
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PROGS = test-ctype
DIRS = de_DE.ISO8859-1 gr_GR.ISO8859-7 koi8-r koi8-to-win1251

all: $(PROGS)

clean: 
	rm -f $(PROGS)
	for d in $(DIRS); do \
		$(MAKE) -C $$d clean; \
	done

check-%: all
	@$(MAKE) -C `echo $@ | sed 's/^check-//'` test
