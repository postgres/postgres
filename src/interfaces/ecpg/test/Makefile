# $PostgreSQL: pgsql/src/interfaces/ecpg/test/Makefile,v 1.67 2007/03/29 12:02:24 meskes Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# port number for temp-installation test postmaster
# this is also defined in test/connect/Makefile
TEMP_PORT = 5$(DEF_PGPORT)

# default encoding
MULTIBYTE = SQL_ASCII

# threading
THREAD := $(shell grep -q "define ENABLE_THREAD_SAFETY" ../include/ecpg_config.h && echo "--enable-threading")

# locale
NOLOCALE =
ifdef NO_LOCALE
NOLOCALE += --no-locale
endif

ifneq ($(PORTNAME),win32)
abs_builddir := $(shell pwd)
else
abs_builddir := $(shell pwd -W)
endif

all install installdirs uninstall distprep:
	$(MAKE) -C connect $@
	$(MAKE) -C expected $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C thread $@

clean distclean maintainer-clean:
	$(MAKE) -C connect $@
	$(MAKE) -C connect extraclean
	$(MAKE) -C expected extraclean
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C thread $@
	rm -rf tmp_check results log
	rm -f pg_regress regression.diffs

all: pg_regress

pg_regress: pg_regress.sh $(top_builddir)/src/Makefile.global
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e 's,@pkglibdir@,$(pkglibdir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@host_tuple@/$(host_tuple)/g' \
	    -e 's,@GMAKE@,$(MAKE),g' \
	    -e 's/@enable_shared@/$(enable_shared)/g' \
	  $< >$@

# When doing a VPATH build, copy over the .pgc, .stdout and .stderr
# files so that the driver script can find them.  We have to use an
# absolute path for the targets, because otherwise make will try to
# locate the missing files using VPATH, and will find them in
# $(srcdir), but the point here is that we want to copy them from
# $(srcdir) to the build directory.

ifdef VPATH
remaining_files_src := $(wildcard $(srcdir)/*/*.pgc) $(wildcard $(srcdir)/expected/*.c) $(wildcard $(srcdir)/expected/*.stdout) $(wildcard $(srcdir)/expected/*.stderr)
remaining_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(remaining_files_src))

all: $(remaining_files_build)
$(remaining_files_build): $(abs_builddir)/%: $(srcdir)/%
	ln -s $< $@
endif


check: all
	sh ./pg_regress  --dbname=regress1 --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE) $(THREAD)

# the same options, but with --listen-on-tcp
checktcp: all
	sh ./pg_regress  --dbname=regress1 --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE) --listen-on-tcp $(THREAD)

installcheck: all
	sh ./pg_regress  --dbname=regress1 --top-builddir=$(top_builddir) --load-language=plpgsql $(NOLOCALE)
