#-------------------------------------------------------------------------
#
# Makefile for the postgres backend
#
# Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL Server"
# This is a daemon process, which is why it is not labeled as an executable
#PGAPPICON=win32

subdir = src/backend
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = access bootstrap catalog parser commands executor foreign lib libpq \
	main nodes optimizer port postmaster regex replication rewrite \
	storage tcop tsearch utils $(top_builddir)/src/timezone

include $(srcdir)/common.mk

# As of 1/2010:
# The probes.o file is necessary for dtrace support on Solaris, and on recent
# versions of systemtap.  (Older systemtap releases just produce an empty
# file, but that's okay.)  However, OS X's dtrace doesn't use it and doesn't
# even recognize the -G option.  So, build probes.o except on Darwin.
# This might need adjustment as other platforms add dtrace support.
ifneq ($(PORTNAME), darwin)
ifeq ($(enable_dtrace), yes)
LOCALOBJS += utils/probes.o
endif
endif

OBJS = $(SUBDIROBJS) $(LOCALOBJS) $(top_builddir)/src/port/libpgport_srv.a \
       $(top_builddir)/src/common/libpgcommon_srv.a

# We put libpgport and libpgcommon into OBJS, so remove it from LIBS; also add
# libldap
LIBS := $(filter-out -lpgport -lpgcommon, $(LIBS)) $(LDAP_LIBS_BE)

# The backend doesn't need everything that's in LIBS, however
LIBS := $(filter-out -lz -lreadline -ledit -ltermcap -lncurses -lcurses, $(LIBS))

##########################################################################

all: submake-libpgport submake-schemapg postgres $(POSTGRES_IMP)

ifneq ($(PORTNAME), cygwin)
ifneq ($(PORTNAME), win32)
ifneq ($(PORTNAME), aix)

postgres: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) $(export_dynamic) $(call expand_subsys,$^) $(LIBS) -o $@

endif
endif
endif

ifeq ($(PORTNAME), cygwin)

postgres: $(OBJS) postgres.def libpostgres.a
	$(DLLTOOL) --dllname $@$(X) --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) -o $@$(X) -Wl,--base-file,$@.base $@.exp $(call expand_subsys,$(OBJS)) $(LIBS)
	$(DLLTOOL) --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) -Wl,--stack,$(WIN32_STACK_RLIMIT) -o $@$(X) $@.exp $(call expand_subsys,$(OBJS)) $(LIBS)
	rm -f $@.exp $@.base

postgres.def: $(OBJS)
	$(DLLTOOL) --export-all --output-def $@ $(call expand_subsys,$^)

libpostgres.a: postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@

endif # cygwin

ifeq ($(PORTNAME), win32)
LIBS += -lsecur32

postgres: $(OBJS) postgres.def libpostgres.a $(WIN32RES)
	$(DLLTOOL) --dllname $@$(X) --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) -o $@$(X) -Wl,--base-file,$@.base $@.exp $(call expand_subsys,$(OBJS)) $(WIN32RES) $(LIBS)
	$(DLLTOOL) --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) -Wl,--stack=$(WIN32_STACK_RLIMIT) -o $@$(X) $@.exp $(call expand_subsys,$(OBJS)) $(WIN32RES) $(LIBS)
	rm -f $@.exp $@.base

postgres.def: $(OBJS)
	$(DLLTOOL) --export-all --output-def $@ $(call expand_subsys,$^)

libpostgres.a: postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@

endif # win32

ifeq ($(PORTNAME), aix)

postgres: $(POSTGRES_IMP)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) $(call expand_subsys,$(OBJS)) -Wl,-bE:$(top_builddir)/src/backend/$(POSTGRES_IMP) $(LIBS) -o $@

$(POSTGRES_IMP): $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(call expand_subsys,$^)
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

# Update the commonly used headers before building the subdirectories
$(SUBDIRS:%=%-recursive): $(top_builddir)/src/include/parser/gram.h $(top_builddir)/src/include/catalog/schemapg.h $(top_builddir)/src/include/utils/fmgroids.h $(top_builddir)/src/include/utils/errcodes.h $(top_builddir)/src/include/utils/probes.h

# run this unconditionally to avoid needing to know its dependencies here:
submake-schemapg:
	$(MAKE) -C catalog schemapg.h

# src/port needs a convenient way to force errcodes.h to get built
submake-errcodes: $(top_builddir)/src/include/utils/errcodes.h

.PHONY: submake-schemapg submake-errcodes

catalog/schemapg.h: | submake-schemapg

$(top_builddir)/src/port/libpgport_srv.a: | submake-libpgport


# The postgres.o target is needed by the rule in Makefile.global that
# creates the exports file when MAKE_EXPORTS = true.
postgres.o: $(OBJS)
	$(CC) $(LDREL) $(LDFLAGS) $(LDFLAGS_EX) $(call expand_subsys,$^) $(LIBS) -o $@


# The following targets are specified in make commands that appear in
# the make files in our subdirectories. Note that it's important we
# match the dependencies shown in the subdirectory makefiles!

parser/gram.h: parser/gram.y
	$(MAKE) -C parser gram.h

utils/fmgroids.h: utils/Gen_fmgrtab.pl catalog/Catalog.pm $(top_srcdir)/src/include/catalog/pg_proc.h
	$(MAKE) -C utils fmgroids.h

utils/errcodes.h: utils/generate-errcodes.pl utils/errcodes.txt
	$(MAKE) -C utils errcodes.h

utils/probes.h: utils/probes.d
	$(MAKE) -C utils probes.h

# Make symlinks for these headers in the include directory. That way
# we can cut down on the -I options. Also, a symlink is automatically
# up to date when we update the base file.
#
# The point of the prereqdir incantation in some of the rules below is to
# force the symlink to use an absolute path rather than a relative path.
# For headers which are generated by make distprep, the actual header within
# src/backend will be in the source tree, while the symlink in src/include
# will be in the build tree, so a simple ../.. reference won't work.
# For headers generated during regular builds, we prefer a relative symlink.

$(top_builddir)/src/include/parser/gram.h: parser/gram.h
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	  cd '$(dir $@)' && rm -f $(notdir $@) && \
	  $(LN_S) "$$prereqdir/$(notdir $<)" .

$(top_builddir)/src/include/catalog/schemapg.h: catalog/schemapg.h
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	  cd '$(dir $@)' && rm -f $(notdir $@) && \
	  $(LN_S) "$$prereqdir/$(notdir $<)" .

$(top_builddir)/src/include/utils/errcodes.h: utils/errcodes.h
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	  cd '$(dir $@)' && rm -f $(notdir $@) && \
	  $(LN_S) "$$prereqdir/$(notdir $<)" .

$(top_builddir)/src/include/utils/fmgroids.h: utils/fmgroids.h
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	  cd '$(dir $@)' && rm -f $(notdir $@) && \
	  $(LN_S) "$$prereqdir/$(notdir $<)" .

$(top_builddir)/src/include/utils/probes.h: utils/probes.h
	cd '$(dir $@)' && rm -f $(notdir $@) && \
	    $(LN_S) "../../../$(subdir)/utils/probes.h" .


utils/probes.o: utils/probes.d $(SUBDIROBJS)
	$(DTRACE) $(DTRACEFLAGS) -C -G -s $(call expand_subsys,$^) -o $@


##########################################################################

# Be sure that these files get removed by the maintainer-clean target
distprep:
	$(MAKE) -C parser	gram.c gram.h scan.c
	$(MAKE) -C bootstrap	bootparse.c bootscanner.c
	$(MAKE) -C catalog	schemapg.h postgres.bki postgres.description postgres.shdescription
	$(MAKE) -C replication	repl_gram.c repl_scanner.c
	$(MAKE) -C utils	fmgrtab.c fmgroids.h errcodes.h
	$(MAKE) -C utils/misc	guc-file.c
	$(MAKE) -C utils/sort	qsort_tuple.c


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
	$(MAKE) -C tsearch install-data
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
	$(INSTALL_PROGRAM) $(MKLDEXPORT) '$(DESTDIR)$(pgxsdir)/$(MKLDEXPORT_DIR)/mkldexport.sh'
endif

.PHONY: install-bin

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)' '$(DESTDIR)$(datadir)'
ifeq ($(PORTNAME), cygwin)
ifeq ($(MAKE_DLL), true)
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
endif
endif
ifeq ($(PORTNAME), win32)
ifeq ($(MAKE_DLL), true)
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
endif
endif
ifeq ($(MAKE_EXPORTS), true)
	$(MKDIR_P) '$(DESTDIR)$(pkglibdir)'
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(MKLDEXPORT_DIR)'
endif


##########################################################################

uninstall:
	rm -f '$(DESTDIR)$(bindir)/postgres$(X)' '$(DESTDIR)$(bindir)/postmaster'
ifeq ($(MAKE_EXPORTS), true)
	rm -f '$(DESTDIR)$(pkglibdir)/$(POSTGRES_IMP)'
	rm -f '$(DESTDIR)$(pgxsdir)/$(MKLDEXPORT_DIR)/mkldexport.sh'
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
	$(MAKE) -C tsearch uninstall-data
	rm -f '$(DESTDIR)$(datadir)/pg_hba.conf.sample' \
	      '$(DESTDIR)$(datadir)/pg_ident.conf.sample' \
              '$(DESTDIR)$(datadir)/postgresql.conf.sample' \
	      '$(DESTDIR)$(datadir)/recovery.conf.sample'


##########################################################################

clean:
	rm -f $(LOCALOBJS) postgres$(X) $(POSTGRES_IMP) \
		$(top_builddir)/src/include/parser/gram.h \
		$(top_builddir)/src/include/catalog/schemapg.h \
		$(top_builddir)/src/include/utils/fmgroids.h \
		$(top_builddir)/src/include/utils/probes.h
ifeq ($(PORTNAME), cygwin)
	rm -f postgres.dll postgres.def libpostgres.a
endif
ifeq ($(PORTNAME), win32)
	rm -f postgres.dll postgres.def libpostgres.a $(WIN32RES)
endif

distclean: clean
	rm -f port/tas.s port/dynloader.c port/pg_sema.c port/pg_shmem.c \
	      port/pg_latch.c

maintainer-clean: distclean
	rm -f bootstrap/bootparse.c \
	      bootstrap/bootscanner.c \
	      parser/gram.c \
	      parser/gram.h \
	      parser/scan.c \
	      catalog/schemapg.h \
	      catalog/postgres.bki \
	      catalog/postgres.description \
	      catalog/postgres.shdescription \
	      replication/repl_gram.c \
	      replication/repl_scanner.c \
	      utils/fmgroids.h \
	      utils/fmgrtab.c \
	      utils/errcodes.h \
	      utils/misc/guc-file.c \
	      utils/sort/qsort_tuple.c


##########################################################################
#
# Support for code development.
#
# Use target "quick" to build "postgres" when you know all the subsystems
# are up to date.  It saves the time of doing all the submakes.
.PHONY: quick
quick: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) $(export_dynamic) $(call expand_subsys,$^) $(LIBS) -o postgres
