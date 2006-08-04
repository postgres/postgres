# $PostgreSQL: pgsql/src/interfaces/ecpg/test/Makefile,v 1.56 2006/08/04 14:18:09 tgl Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# port number for temp-installation test postmaster
# this is also defined in test/connect/Makefile
TEMP_PORT = 5$(DEF_PGPORT)

# default encoding
MULTIBYTE = SQL_ASCII

# locale
NOLOCALE :=
ifdef NO_LOCALE
NOLOCALE += --no-locale
endif

all install installdirs uninstall dep depend distprep:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C errors $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C complex $@
	$(MAKE) -C thread $@

clean distclean maintainer-clean:
	$(MAKE) -C connect $@
	$(MAKE) -C connect extraclean
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C errors $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C complex $@
	$(MAKE) -C thread $@
	rm -rf tmp_check results log
	rm -f pg_regress.inc.sh regression.diff

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

check: all pg_regress.inc.sh
	sh ./pg_regress.sh  --dbname=regress1 --debug --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE)

# the same options, but with --listen-on-tcp
checktcp: all pg_regress.inc.sh
	sh ./pg_regress.sh  --dbname=regress1 --debug --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE) --listen-on-tcp

