#-------------------------------------------------------------------------
#
# Makefile for the postgres backend
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/backend/Makefile,v 1.95 2003/03/21 17:18:34 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := access bootstrap catalog parser commands executor lib libpq \
	main nodes optimizer port postmaster regex rewrite \
	storage tcop utils

OBJS := $(DIRS:%=%/SUBSYS.o)

ifeq ($(PORTNAME), qnx4)
# This file crashes qnx4's wlink and is therefore not in
# bootstrap/SUBSYS.o on that platform. (Wotta hack ... is it still
# necessary?) [ Yes, until the Watcom compiler goes open source it's
# effectively unsupported ]
OBJS+= bootstrap/bootstrap.o
endif


##########################################################################

all: submake-libpgport postgres $(POSTGRES_IMP)

ifneq ($(PORTNAME), cygwin)

postgres: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(export_dynamic) $^ $(LIBS) -o $@

else # cygwin

postgres: $(OBJS) $(DLLINIT) postgres.def libpostgres.a
	$(DLLTOOL) --dllname $@$(X) --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@$(X) -Wl,--base-file,$@.base $@.exp $(OBJS) $(LIBS)
	$(DLLTOOL) --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@$(X) $@.exp $(OBJS) $(LIBS)
	rm -f $@.exp $@.base

postgres.def: $(OBJS)
	$(DLLTOOL) --export-all --output-def $@ $^

libpostgres.a: postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@

endif # cygwin

ifeq ($(PORTNAME), aix)

postgres: $(POSTGRES_IMP)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -Wl,-bE:$(top_builddir)/src/backend/$(POSTGRES_IMP) $(LIBS) -o $@

$(POSTGRES_IMP): $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $^
ifeq ($(host_os), aix3.2.5)
	$(MKLDEXPORT) SUBSYS.o $(bindir)/postgres > $@
else
ifneq (,$(findstring aix4.1, $(host_os)))
	$(MKLDEXPORT) SUBSYS.o $(bindir)/postgres > $@
else
	$(MKLDEXPORT) SUBSYS.o . > $@
endif
endif
	@rm -f SUBSYS.o

endif # aix

# Parallel make trickery
$(OBJS): $(DIRS:%=%-recursive) ;

.PHONY: $(DIRS:%=%-recursive)
# Update the commonly used headers before building the subdirectories
$(DIRS:%=%-recursive): $(top_builddir)/src/include/parser/parse.h $(top_builddir)/src/include/utils/fmgroids.h
	$(MAKE) -C $(subst -recursive,,$@) all


$(DLLINIT): $(DLLINIT:%.o=%.c)
	$(MAKE) -C $(@D) $(@F)

# The postgres.o target is needed by the rule in Makefile.global that
# creates the exports file when MAKE_EXPORTS = true.
postgres.o: $(OBJS)
	$(CC) $(LDREL) $(LDFLAGS) $^ $(LIBS) -o $@


# The following targets are specified in make commands that appear in
# the make files in our subdirectories. Note that it's important we
# match the dependencies shown in the subdirectory makefiles!

$(srcdir)/parser/parse.h: parser/gram.y
	$(MAKE) -C parser parse.h

utils/fmgroids.h: utils/Gen_fmgrtab.sh $(top_srcdir)/src/include/catalog/pg_proc.h
	$(MAKE) -C utils fmgroids.h

# Make symlinks for these headers in the include directory. That way
# we can cut down on the -I options. Also, a symlink is automatically
# up to date when we update the base file.

$(top_builddir)/src/include/parser/parse.h: $(srcdir)/parser/parse.h
	prereqdir=`cd $(dir $<) >/dev/null && pwd` && \
	  cd $(dir $@) && rm -f $(notdir $@) && \
	  $(LN_S) "$$prereqdir/$(notdir $<)" .

$(top_builddir)/src/include/utils/fmgroids.h: utils/fmgroids.h
	cd $(dir $@) && rm -f $(notdir $@) && \
	    $(LN_S) ../../../$(subdir)/utils/fmgroids.h .


##########################################################################

distprep:
	$(MAKE) -C parser	gram.c parse.h scan.c
	$(MAKE) -C bootstrap	bootparse.c bootstrap_tokens.h bootscanner.c
	$(MAKE) -C utils/misc	guc-file.c


##########################################################################

install: all installdirs install-bin
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	$(INSTALL_DATA) libpostgres.a $(DESTDIR)$(libdir)/libpostgres.a
endif
endif
	$(MAKE) -C catalog install-data
	$(INSTALL_DATA) $(srcdir)/libpq/pg_hba.conf.sample $(DESTDIR)$(datadir)/pg_hba.conf.sample
	$(INSTALL_DATA) $(srcdir)/libpq/pg_ident.conf.sample $(DESTDIR)$(datadir)/pg_ident.conf.sample
	$(INSTALL_DATA) $(srcdir)/utils/misc/postgresql.conf.sample $(DESTDIR)$(datadir)/postgresql.conf.sample

install-bin: postgres $(POSTGRES_IMP) installdirs
	$(INSTALL_PROGRAM) postgres$(X) $(DESTDIR)$(bindir)/postgres$(X)
	@rm -f $(DESTDIR)$(bindir)/postmaster
	ln -s postgres$(X) $(DESTDIR)$(bindir)/postmaster
ifeq ($(MAKE_EXPORTS), true)
	$(INSTALL_DATA) $(POSTGRES_IMP) $(DESTDIR)$(pkglibdir)/$(POSTGRES_IMP)
endif

.PHONY: install-bin

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	$(mkinstalldirs) $(DESTDIR)$(libdir)
endif
endif
ifeq ($(MAKE_EXPORTS), true)
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)
endif


##########################################################################

uninstall:
	rm -f $(DESTDIR)$(bindir)/postgres$(X) $(DESTDIR)$(bindir)/postmaster
ifeq ($(MAKE_EXPORTS), true)
	rm -f $(DESTDIR)$(pkglibdir)/$(POSTGRES_IMP)
endif
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	rm -f $(DESTDIR)$(libdir)/libpostgres.a
endif
endif
	$(MAKE) -C catalog uninstall-data
	rm -f $(DESTDIR)$(datadir)/pg_hba.conf.sample \
	      $(DESTDIR)$(datadir)/pg_service.conf.sample \
	      $(DESTDIR)$(datadir)/pg_ident.conf.sample \
              $(DESTDIR)$(datadir)/postgresql.conf.sample


##########################################################################

clean:
	rm -f postgres$(X) $(POSTGRES_IMP) \
		$(top_srcdir)/src/include/parser/parse.h \
		$(top_builddir)/src/include/utils/fmgroids.h
ifeq ($(PORTNAME), cygwin)
	rm -f postgres.dll postgres.def libpostgres.a
endif
	for i in $(DIRS); do $(MAKE) -C $$i clean || exit; done

distclean: clean
	rm -f port/tas.s port/dynloader.c port/pg_sema.c port/pg_shmem.c

maintainer-clean: distclean
	rm -f $(srcdir)/bootstrap/bootparse.c \
	      $(srcdir)/bootstrap/bootscanner.c \
	      $(srcdir)/bootstrap/bootstrap_tokens.h \
	      $(srcdir)/parser/gram.c \
	      $(srcdir)/parser/scan.c \
	      $(srcdir)/parser/parse.h \
	      $(srcdir)/utils/misc/guc-file.c


##########################################################################
#
# Support for code development.
#
# Use target "quick" to build "postgres" when you know all the subsystems 
# are up to date.  It saves the time of doing all the submakes.
.PHONY: quick
quick: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(export_dynamic) $^ $(LIBS) -o postgres

depend dep: $(top_srcdir)/src/include/parser/parse.h $(top_builddir)/src/include/utils/fmgroids.h
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
