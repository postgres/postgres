#-------------------------------------------------------------------------
#
# Makefile for backend/utils
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/utils/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS        = fmgrtab.o
SUBDIRS     = \
	activity \
	adt \
	cache \
	error \
	fmgr \
	hash \
	init \
	mb \
	misc \
	mmgr \
	resowner \
	sort \
	time

# location of Catalog.pm
catalogdir  = $(top_srcdir)/src/backend/catalog

include $(top_srcdir)/src/backend/common.mk

all: probes.h generated-header-symlinks

.PHONY: generated-header-symlinks submake-adt-headers

generated-header-symlinks: $(top_builddir)/src/include/utils/header-stamp submake-adt-headers

submake-adt-headers:
	$(MAKE) -C adt jsonpath_gram.h

$(SUBDIRS:%=%-recursive): fmgr-stamp errcodes.h

# fmgr-stamp records the last time we ran Gen_fmgrtab.pl.  We don't rely on
# the timestamps of the individual output files, because the Perl script
# won't update them if they didn't change (to avoid unnecessary recompiles).
fmgr-stamp: Gen_fmgrtab.pl $(catalogdir)/Catalog.pm $(top_srcdir)/src/include/catalog/pg_proc.dat
	$(PERL) -I $(catalogdir) $< --include-path=$(top_srcdir)/src/include/ $(top_srcdir)/src/include/catalog/pg_proc.dat
	touch $@

errcodes.h: $(top_srcdir)/src/backend/utils/errcodes.txt generate-errcodes.pl
	$(PERL) $(srcdir)/generate-errcodes.pl --outfile $@ $<

ifeq ($(enable_dtrace), yes)
probes.h: postprocess_dtrace.sed probes.h.tmp
	sed -f $^ >$@
	rm probes.h.tmp

probes.h.tmp: probes.d
	$(DTRACE) -C -h -s $< -o $@
else
probes.h: Gen_dummy_probes.pl probes.d
	$(PERL) -n $^ >$@
endif

# These generated headers must be symlinked into src/include/.
# We use header-stamp to record that we've done this because the symlinks
# themselves may appear older than fmgr-stamp.
$(top_builddir)/src/include/utils/header-stamp: fmgr-stamp errcodes.h probes.h
	cd '$(dir $@)' && for file in fmgroids.h fmgrprotos.h errcodes.h probes.h; do \
	  rm -f $$file && $(LN_S) "../../../$(subdir)/$$file" . ; \
	done
	touch $@

.PHONY: install-data
install-data: errcodes.txt installdirs
	$(INSTALL_DATA) $(srcdir)/errcodes.txt '$(DESTDIR)$(datadir)/errcodes.txt'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

.PHONY: uninstall-data
uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)'/, errcodes.txt)

clean:
	rm -f probes.h probes.h.tmp
	rm -f fmgroids.h fmgrprotos.h fmgrtab.c fmgr-stamp errcodes.h
