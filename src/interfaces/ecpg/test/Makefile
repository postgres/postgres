# $PostgreSQL: pgsql/src/interfaces/ecpg/test/Makefile,v 1.54 2006/08/02 13:43:23 meskes Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# port number for temp-installation test postmaster
TEMP_PORT = 5$(DEF_PGPORT)

# default encoding
MULTIBYTE = SQL_ASCII

# locale
NOLOCALE :=
ifdef NO_LOCALE
NOLOCALE += --no-locale
endif

all clean install installdirs uninstall dep depend distprep:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C errors $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C complex $@
	$(MAKE) -C thread $@
# for some reason I couldn't figure out, ifeq($@,clean) ... does not work
	if [ $@ = clean ]; then rm -f results/*.stdout results/*.stderr results/*.c; rm -rf tmp_check/; rm -f log/*.log; rm -f pg_regress.inc.sh regression.diff; fi

all: pg_regress.sh

pg_regress.sh: pg_regress.inc.sh

pg_regress.inc.sh: pg_regress.inc.sh.in $(top_builddir)/src/Makefile.global
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e 's,@pkglibdir@,$(pkglibdir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@host_tuple@/$(host_tuple)/g' \
	    -e 's,@GMAKE@,$(MAKE),g' \
	    -e 's/@enable_shared@/$(enable_shared)/g' \
	    -e 's/@GCC@/$(GCC)/g' \
	  $< >$@

test: all pg_regress.inc.sh
	sh ./pg_regress.sh  --dbname=regress1 --debug --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --listen-on-tcp --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE)

check: all test

