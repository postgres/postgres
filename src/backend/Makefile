#-------------------------------------------------------------------------
#
# Makefile--
#   Makefile for the postgres backend (and the postmaster)
#
# Copyright (c) 1994, Regents of the University of California
#
# Functional notes:
#
#   Parallel make:  
#
#     This make file is set up so that you can do a parallel make (with 
#     the --jobs option of make) and make multiple subdirectories at 
#     once.  
#
#     However, the subdirectory make files are not so careful.
#     Normally, the --jobs option would get passed down to those
#     subdirectory makes, like any other make option, and they would
#     fail.  But there's a trick: Put a value (max number of
#     processes) on the --jobs option, e.g. --jobs=4.  Now, due to a
#     special feature of make, the --jobs option will not get passed
#     to the subdirectory makes.  (make does this because if you only
#     want 4 tasks running, then splitting the subdirectory makes into
#     multiple tasks would violate your wishes).
#
#
#
# Implementation notes:
#
#   We don't use $(LD) for linking.  We use $(CC) instead.  This is because
#   the $(CC) program apparently can do linking too, and it has certain
#   thinks like default options and search paths for libraries set up for 
#   it that the more primitive $(LD) doesn't have.
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/Makefile,v 1.38 1999/03/07 23:05:56 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ..
include ../Makefile.global

DIRS = access bootstrap catalog commands executor lib libpq \
	main parser nodes optimizer port postmaster regex rewrite \
	storage tcop utils

ifdef TIOGA
DIRS += tioga
endif

OBJS = $(DIRS:%=%/SUBSYS.o)

# kerberos flags

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
LDFLAGS+= $(KRBLIBS)
endif

ifeq ($(MAKE_DLL), true)
DLLOBJS=$(OBJS)
DLLOBJS+= ../utils/version.o
DLLLIBS= -L/usr/local/lib -lcygipc -lcrypt -lcygwin -lkernel32

postgres.def: $(DLLOBJS)
	$(DLLTOOL) --export-all --output-def $@ $(DLLOBJS)

libpostgres.a: $(DLLOBJS) ../utils/dllinit.o postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@
endif

all: postgres $(POSTGRES_IMP) global1.bki.source local1_template1.bki.source \
				global1.description local1_template1.description

ifneq ($(PORTNAME), win)
postgres: fmgr.h $(OBJS) ../utils/version.o
	$(CC) -o postgres $(OBJS) ../utils/version.o $(LDFLAGS)
else
postgres: $(DLLOBJS) ../utils/dllinit.o postgres.def libpostgres.a
	dlltool --dllname $@$(X) --output-exp $@.exp --def postgres.def
	gcc -g -o $@$(X) -Wl,--base-file,$@.base $@.exp $(DLLOBJS) $(DLLLIBS)
	dlltool --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	gcc -g -o $@$(X) $@.exp $(DLLOBJS) $(DLLLIBS)
	rm $@.exp $@.base
endif

#.PHONY: postgres

$(OBJS): $(DIRS:%=%.dir)

$(DIRS:%=%.dir):
	$(MAKE) -C $(subst .dir,,$@) all 

../utils/version.o:
	$(MAKE) -C ../utils version.o

../utils/dllinit.c:
	$(MAKE) -C ../utils dllinit.o

global1.bki.source local1_template1.bki.source \
global1.description local1_template1.description: catalog/$@
	cp catalog/$@ .

catalog/global1.bki.source catalog/local1_template1.bki.source \
catalog/global1.description catalog/local1_template1.description:
	$(MAKE) -C catalog $@

# The postgres.o target is needed by the rule in Makefile.global that
# creates the exports file when MAKE_EXPORTS = true.
postgres.o: $(OBJS)
	$(CC) -r -o postgres.o $(OBJS) $(LDFLAGS)

############################################################################
# The following targets are specified in make commands that appear in the
# make files in our subdirectories.

parse.h: parser/parse.h
	$(MAKE) -C parser parse.h
	cp parser/parse.h .

fmgr.h:
	$(MAKE) -C utils fmgr.h
	cp utils/fmgr.h .

#############################################################################
clean:
	rm -f postgres$(X) $(POSTGRES_IMP) fmgr.h parse.h \
	    global1.bki.source local1_template1.bki.source \
	    global1.description local1_template1.description
ifeq ($(PORTNAME), win)
ifeq ($(MAKE_DLL), true)
	rm -f postgres.dll postgres.def libpostgres.a
endif
endif
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

.DEFAULT:
	for i in $(DIRS); do $(MAKE) -C $$i $@; done

#############################################################################
#
# Installation.
#
# Install the backend program (postgres) to the binary directory and 
# make a link as "postmaster".  Install the bki files, templates, and sample
# files to the library directory.  Install exported headers to the include
# directory (these headers are the minimal ones needed to build loadable
# backend extensions).
#
# (History:  Before Release 2, make install generated a bki.source file
# and then used build parameters to convert it to a bki file, then installed
# that bki file in the /files subdirectory of the default data directory.
# Initdb then used the bki file to generate the database catalog classes.
# That had to change because (1) there can be more than one database system,
# and (2) the parameters of a database system should be set at initdb time,
# not at postgres build time.

.PHONY: install install-bin install-lib install-headers

install: $(LIBDIR) $(BINDIR) $(HEADERDIR) postgres $(POSTGRES_IMP) \
         install-bin install-lib install-headers

install-bin: $(BINDIR) postgres$(X) $(POSTGRES_IMP)
	$(INSTALL) $(INSTL_EXE_OPTS) postgres$(X) $(BINDIR)/postgres$(X)
	@rm -f $(BINDIR)/postmaster
	ln -s postgres$(X) $(BINDIR)/postmaster
ifeq ($(MAKE_EXPORTS), true)
	$(INSTALL) $(INSTLOPTS) $(POSTGRES_IMP) $(LIBDIR)/$(POSTGRES_IMP)
endif
ifeq ($(PORTNAME), win)
ifeq ($(MAKE_DLL), true)
#	$(INSTALL) $(INSTLOPTS) postgres.dll $(BINDIR)/postgres.dll
	$(INSTALL) $(INSTLOPTS) libpostgres.a $(LIBDIR)/libpostgres.a
endif
endif

install-lib: $(LIBDIR) \
         global1.bki.source local1_template1.bki.source \
         global1.description local1_template1.description \
         libpq/pg_hba.conf.sample optimizer/geqo/pg_geqo.sample
	$(INSTALL) $(INSTLOPTS) global1.bki.source \
	  $(LIBDIR)/global1.bki.source
	$(INSTALL) $(INSTLOPTS) global1.description \
	  $(LIBDIR)/global1.description
	$(INSTALL) $(INSTLOPTS) local1_template1.bki.source \
	  $(LIBDIR)/local1_template1.bki.source
	$(INSTALL) $(INSTLOPTS) local1_template1.description \
	  $(LIBDIR)/local1_template1.description
	$(INSTALL) $(INSTLOPTS) libpq/pg_hba.conf.sample \
	  $(LIBDIR)/pg_hba.conf.sample
	$(INSTALL) $(INSTLOPTS) optimizer/geqo/pg_geqo.sample \
	  $(LIBDIR)/pg_geqo.sample

install-headers: fmgr.h $(SRCDIR)/include/config.h
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi
	@if [ ! -d $(HEADERDIR)/port ]; then mkdir $(HEADERDIR)/port; fi
	@if [ ! -d $(HEADERDIR)/port/$(PORTNAME) ]; \
		then mkdir $(HEADERDIR)/port/$(PORTNAME); fi
	@if [ ! -d $(HEADERDIR)/lib ]; \
		then mkdir $(HEADERDIR)/lib; fi
	@if [ ! -d $(HEADERDIR)/libpq ]; \
		then mkdir $(HEADERDIR)/libpq; fi
	@if [ ! -d $(HEADERDIR)/utils ]; \
		then mkdir $(HEADERDIR)/utils; fi
	@if [ ! -d $(HEADERDIR)/access ]; \
		then mkdir $(HEADERDIR)/access; fi
	@if [ ! -d $(HEADERDIR)/executor ]; \
		then mkdir $(HEADERDIR)/executor; fi
	@if [ ! -d $(HEADERDIR)/commands ]; \
		then mkdir $(HEADERDIR)/commands; fi
	$(INSTALL) $(INSTLOPTS) fmgr.h \
          $(HEADERDIR)/fmgr.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/os.h \
          $(HEADERDIR)/os.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/config.h \
          $(HEADERDIR)/config.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/c.h \
          $(HEADERDIR)/c.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/postgres.h \
          $(HEADERDIR)/postgres.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/postgres_ext.h \
          $(HEADERDIR)/postgres_ext.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/libpq/pqcomm.h \
          $(HEADERDIR)/libpq/pqcomm.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/libpq/libpq-fs.h \
          $(HEADERDIR)/libpq/libpq-fs.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/lib/dllist.h \
          $(HEADERDIR)/lib/dllist.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/utils/geo_decls.h \
          $(HEADERDIR)/utils/geo_decls.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/utils/elog.h \
          $(HEADERDIR)/utils/elog.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/utils/palloc.h \
          $(HEADERDIR)/utils/palloc.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/utils/mcxt.h \
          $(HEADERDIR)/utils/mcxt.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/access/attnum.h \
          $(HEADERDIR)/access/attnum.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/executor/spi.h \
          $(HEADERDIR)/executor/spi.h
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/include/commands/trigger.h \
          $(HEADERDIR)/commands/trigger.h
ifeq ($(PORTNAME), hpux)
# is this still necessary?
	$(INSTALL) $(INSTLOPTS) $(SRCDIR)/backend/port/hpux/fixade.h \
          $(HEADERDIR)/port/hpux/fixade.h
endif


$(BINDIR):
	mkdir $@
$(LIBDIR):
	mkdir $@
$(HEADERDIR):
	mkdir $@

#############################################################################
#
# Support for code development.
#
# Use target "quick" to build "postgres" when you know all the subsystems 
# are up to date.  It saves the time of doing all the submakes.
.PHONY: quick
quick: $(OBJS)
	$(CC) -o postgres $(OBJS) $(LDFLAGS)

#
# Build the file, "./ID", used by the "gid" (grep-for-identifier) tool
#
IDFILE=	ID
.PHONY: $(IDFILE)
$(IDFILE):
	./makeID 

#
# Special rule to generate cpp'd version of a .c file.  This is
# especially useful given all the hellish macro processing going on.
# The cpp'd version has a .C suffix.  To create foo.C from foo.c, just
# type
#	make foo.C
#
%.cpp:	%.c
	$(CC) -E $(CFLAGS) $(<:.C=.c) | cat -s | cb | tr -s '\012*' '\012' \
	    > $(@F)
