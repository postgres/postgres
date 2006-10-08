#-------------------------------------------------------------------------
#
# Makefile for the postgres backend
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/backend/Makefile,v 1.120 2006/10/08 17:15:33 tgl Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL Server"
subdir = src/backend
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := access bootstrap catalog parser commands executor lib libpq \
	main nodes optimizer port postmaster regex rewrite \
	storage tcop utils $(top_builddir)/src/timezone

SUBSYSOBJS := $(DIRS:%=%/SUBSYS.o)

ifeq ($(enable_dtrace), yes)
LOCALOBJS += utils/probes.o
endif

OBJS = $(SUBSYSOBJS) $(LOCALOBJS) $(top_builddir)/src/port/libpgport_srv.a

# We put libpgport into OBJS, so remove it from LIBS; also add libldap
LIBS := $(filter-out -lpgport, $(LIBS)) $(LDAP_LIBS_BE)

# The backend doesn't need everything that's in LIBS, however
LIBS := $(filter-out -lz -lreadline -ledit -ltermcap -lncurses -lcurses, $(LIBS))

##########################################################################

all: submake-libpgport postgres $(POSTGRES_IMP)

ifneq ($(PORTNAME), cygwin)
ifneq ($(PORTNAME), win32)
ifneq ($(PORTNAME), aix)

postgres: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(export_dynamic) $^ $(LIBS) -o $@

endif
endif
endif

ifeq ($(PORTNAME), cygwin)

postgres: $(OBJS) postgres.def libpostgres.a
	$(DLLTOOL) --dllname $@$(X) --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@$(X) -Wl,--base-file,$@.base $@.exp $(OBJS) $(LIBS)
	$(DLLTOOL) --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--stack,$(WIN32_STACK_RLIMIT) -o $@$(X) $@.exp $(OBJS) $(LIBS)
	rm -f $@.exp $@.base

postgres.def: $(OBJS)
	$(DLLTOOL) --export-all --output-def $@ $^

libpostgres.a: postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@

endif # cygwin

ifeq ($(PORTNAME), win32)

postgres: $(OBJS) postgres.def libpostgres.a $(WIN32RES)
	$(DLLTOOL) --dllname $@$(X) --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@$(X) -Wl,--base-file,$@.base $@.exp $(OBJS) $(WIN32RES) $(LIBS)
	$(DLLTOOL) --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--stack=$(WIN32_STACK_RLIMIT) -o $@$(X) $@.exp $(OBJS) $(WIN32RES) $(LIBS)
	rm -f $@.exp $@.base

postgres.def: $(OBJS)
	$(DLLTOOL) --export-all --output-def $@ $^

libpostgres.a: postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@

endif # win32

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
$(SUBSYSOBJS): $(DIRS:%=%-recursive) ;

.PHONY: $(DIRS:%=%-recursive)
# Update the commonly used headers before building the subdirectories
$(DIRS:%=%-recursive): $(top_builddir)/src/include/parser/parse.h $(top_builddir)/src/include/utils/fmgroids.h
	$(MAKE) -C $(subst -recursive,,$@) all


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


utils/probes.o: utils/probes.d $(SUBSYSOBJS)
	$(DTRACE) $(DTRACEFLAGS) -G -s $^ -o $@


##########################################################################

distprep:
	$(MAKE) -C parser	gram.c parse.h scan.c
	$(MAKE) -C bootstrap	bootparse.c bootstrap_tokens.h bootscanner.c
	$(MAKE) -C utils/misc	guc-file.c


##########################################################################

install: all installdirs install-bin
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	$(INSTALL_DATA) libpostgres.a '$(DESTDIR)$(libdir)/libpostgres.a'
endif
endif
ifeq ($(PORTNAME), win32)
ifeq ($(MAKE_DLL), true)
	$(INSTALL_DATA) libpostgres.a '$(DESTDIR)$(libdir)/libpostgres.a'
endif
endif
	$(MAKE) -C catalog install-data
	$(INSTALL_DATA) $(srcdir)/libpq/pg_hba.conf.sample '$(DESTDIR)$(datadir)/pg_hba.conf.sample'
	$(INSTALL_DATA) $(srcdir)/libpq/pg_ident.conf.sample '$(DESTDIR)$(datadir)/pg_ident.conf.sample'
	$(INSTALL_DATA) $(srcdir)/utils/misc/postgresql.conf.sample '$(DESTDIR)$(datadir)/postgresql.conf.sample'
	$(INSTALL_DATA) $(srcdir)/access/transam/recovery.conf.sample '$(DESTDIR)$(datadir)/recovery.conf.sample'

install-bin: postgres $(POSTGRES_IMP) installdirs
	$(INSTALL_PROGRAM) postgres$(X) '$(DESTDIR)$(bindir)/postgres$(X)'
ifneq ($(PORTNAME), win32)
	@rm -f '$(DESTDIR)$(bindir)/postmaster$(X)'
	ln -s postgres$(X) '$(DESTDIR)$(bindir)/postmaster$(X)'
else
	$(INSTALL_PROGRAM) postgres$(X) '$(DESTDIR)$(bindir)/postmaster$(X)'
endif
ifeq ($(MAKE_EXPORTS), true)
	$(INSTALL_DATA) $(POSTGRES_IMP) '$(DESTDIR)$(pkglibdir)/$(POSTGRES_IMP)'
endif

.PHONY: install-bin

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(datadir)'
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	$(mkinstalldirs) '$(DESTDIR)$(libdir)'
endif
endif
ifeq ($(PORTNAME), win32)
ifeq ($(MAKE_DLL), true)
	$(mkinstalldirs) '$(DESTDIR)$(libdir)'
endif
endif
ifeq ($(MAKE_EXPORTS), true)
	$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)'
endif


##########################################################################

uninstall:
	rm -f '$(DESTDIR)$(bindir)/postgres$(X)' '$(DESTDIR)$(bindir)/postmaster'
ifeq ($(MAKE_EXPORTS), true)
	rm -f '$(DESTDIR)$(pkglibdir)/$(POSTGRES_IMP)'
endif
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	rm -f '$(DESTDIR)$(libdir)/libpostgres.a'
endif
endif
ifeq ($(PORTNAME), win32)
ifeq ($(MAKE_DLL), true)
	rm -f '$(DESTDIR)$(libdir)/libpostgres.a'
endif
endif
	$(MAKE) -C catalog uninstall-data
	rm -f '$(DESTDIR)$(datadir)/pg_hba.conf.sample' \
	      '$(DESTDIR)$(datadir)/pg_ident.conf.sample' \
              '$(DESTDIR)$(datadir)/postgresql.conf.sample' \
	      '$(DESTDIR)$(datadir)/recovery.conf.sample'


##########################################################################

clean:
	rm -f $(LOCALOBJS) postgres$(X) $(POSTGRES_IMP) \
		$(top_srcdir)/src/include/parser/parse.h \
		$(top_builddir)/src/include/utils/fmgroids.h
ifeq ($(PORTNAME), cygwin)
	rm -f postgres.dll postgres.def libpostgres.a
endif
ifeq ($(PORTNAME), win32)
	rm -f postgres.dll postgres.def libpostgres.a $(WIN32RES)
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
