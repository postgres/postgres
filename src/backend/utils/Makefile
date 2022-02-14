#-------------------------------------------------------------------------
#
# Makefile for backend/utils
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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

all: distprep probes.h generated-header-symlinks

distprep: fmgr-stamp errcodes.h

.PHONY: generated-header-symlinks

generated-header-symlinks: $(top_builddir)/src/include/utils/header-stamp $(top_builddir)/src/include/utils/probes.h

$(SUBDIRS:%=%-recursive): fmgr-stamp errcodes.h

# fmgr-stamp records the last time we ran Gen_fmgrtab.pl.  We don't rely on
# the timestamps of the individual output files, because the Perl script
# won't update them if they didn't change (to avoid unnecessary recompiles).
fmgr-stamp: Gen_fmgrtab.pl $(catalogdir)/Catalog.pm $(top_srcdir)/src/include/catalog/pg_proc.dat
	$(PERL) -I $(catalogdir) $< --include-path=$(top_srcdir)/src/include/ $(top_srcdir)/src/include/catalog/pg_proc.dat
	touch $@

errcodes.h: $(top_srcdir)/src/backend/utils/errcodes.txt generate-errcodes.pl
	$(PERL) $(srcdir)/generate-errcodes.pl $< > $@

ifneq ($(enable_dtrace), yes)
probes.h: Gen_dummy_probes.sed
endif

# We editorialize on dtrace's output to the extent of changing the macro
# names (from POSTGRESQL_foo to TRACE_POSTGRESQL_foo) and changing any
# "char *" arguments to "const char *".
probes.h: probes.d
ifeq ($(enable_dtrace), yes)
	$(DTRACE) -C -h -s $< -o $@.tmp
	sed -e 's/POSTGRESQL_/TRACE_POSTGRESQL_/g' \
	    -e 's/( *char \*/(const char */g' \
	    -e 's/, *char \*/, const char */g' $@.tmp >$@
	rm $@.tmp
else
	sed -f $(srcdir)/Gen_dummy_probes.sed $< >$@
endif

# These generated headers must be symlinked into builddir/src/include/,
# using absolute links for the reasons explained in src/backend/Makefile.
# We use header-stamp to record that we've done this because the symlinks
# themselves may appear older than fmgr-stamp.
$(top_builddir)/src/include/utils/header-stamp: fmgr-stamp errcodes.h
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	cd '$(dir $@)' && for file in fmgroids.h fmgrprotos.h errcodes.h; do \
	  rm -f $$file && $(LN_S) "$$prereqdir/$$file" . ; \
	done
	touch $@

# probes.h is handled differently because it's not in the distribution tarball.
$(top_builddir)/src/include/utils/probes.h: probes.h
	cd '$(dir $@)' && rm -f $(notdir $@) && \
	    $(LN_S) "../../../$(subdir)/probes.h" .

# Recipe for rebuilding the Perl version of Gen_dummy_probes
# Nothing depends on it, so it will never be called unless explicitly requested
# The last two lines of the recipe format the script according to  our
# standard and put back some blank lines for improved readability.
Gen_dummy_probes.pl: Gen_dummy_probes.sed Gen_dummy_probes.pl.prolog
	cp $(srcdir)/Gen_dummy_probes.pl.prolog $@
	s2p -f $<  | sed -e 1,3d -e '/# #/ d' -e '$$d' >> $@
	perltidy --profile=$(srcdir)/../../tools/pgindent/perltidyrc $@
	perl -pi -e '!$$lb && ( /^\t+#/  || /^# prototypes/ ) && print qq{\n};'\
		-e '$$lb = m/^\n/; ' $@

.PHONY: install-data
install-data: errcodes.txt installdirs
	$(INSTALL_DATA) $(srcdir)/errcodes.txt '$(DESTDIR)$(datadir)/errcodes.txt'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

.PHONY: uninstall-data
uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)'/, errcodes.txt)

# fmgroids.h, fmgrprotos.h, fmgrtab.c, fmgr-stamp, and errcodes.h are in the
# distribution tarball, so they are not cleaned here.
clean:
	rm -f probes.h

maintainer-clean: clean
	rm -f fmgroids.h fmgrprotos.h fmgrtab.c fmgr-stamp errcodes.h
