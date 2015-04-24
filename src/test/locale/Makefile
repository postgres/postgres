# src/test/locale/Makefile

subdir = src/test/locale
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PROGS = test-ctype
DIRS = de_DE.ISO8859-1 gr_GR.ISO8859-7 koi8-r koi8-to-win1251

all: $(PROGS)

clean distclean maintainer-clean:
	rm -f $(PROGS) *.o
	for d in $(DIRS); do \
		$(MAKE) -C $$d clean || exit; \
	done

check-%: all
	@$(MAKE) -C `echo $@ | sed 's/^check-//'` test
