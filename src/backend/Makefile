#-------------------------------------------------------------------------
#
# Makefile for the postgres backend
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/backend/Makefile,v 1.65 2000/09/17 13:02:30 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := access bootstrap catalog parser commands executor lib libpq \
	main nodes optimizer port postmaster regex rewrite \
	storage tcop utils

ifdef TIOGA
DIRS += tioga
endif

OBJS := $(DIRS:%=%/SUBSYS.o)

ifeq ($(PORTNAME), qnx4)
# This file crashes qnx4's wlink and is therefore not in
# bootstrap/SUBSYS.o on that platform. (Wotta hack ... is it still
# necessary?)
OBJS+= bootstrap/bootstrap.o
endif


##########################################################################

all: postgres $(POSTGRES_IMP)

ifneq ($(PORTNAME), win)

postgres: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

else # win

# No points for style here. How about encapsulating some of these
# commands into variables?
postgres: $(DLLOBJS) $(top_builddir)/src/utils/dllinit.o postgres.def libpostgres.a
	dlltool --dllname $@$(X) --output-exp $@.exp --def postgres.def
	gcc -g -o $@$(X) -Wl,--base-file,$@.base $@.exp $(DLLOBJS) $(DLLLIBS)
	dlltool --dllname $@$(X) --base-file $@.base --output-exp $@.exp --def postgres.def
	gcc -g -o $@$(X) $@.exp $(DLLOBJS) $(DLLLIBS)
	rm $@.exp $@.base

endif # win


# Parallel make trickery
$(OBJS): $(DIRS:%=%-recursive)

.PHONY: $(DIRS:%=%-recursive)
# Update the commonly used headers before building the subdirectories
$(DIRS:%=%-recursive): $(top_srcdir)/src/include/parser/parse.h $(top_builddir)/src/include/utils/fmgroids.h
	$(MAKE) -C $(subst -recursive,,$@) all


ifeq ($(MAKE_DLL), true)
DLLOBJS := $(OBJS)
DLLLIBS := -L/usr/local/lib -lcygipc -lcrypt -lcygwin -lkernel32

postgres.def: $(DLLOBJS)
	$(DLLTOOL) --export-all --output-def $@ $(DLLOBJS)

libpostgres.a: $(DLLOBJS) $(top_builddir)/src/utils/dllinit.o postgres.def
	$(DLLTOOL) --dllname postgres.exe --def postgres.def --output-lib $@
endif # MAKE_DLL


$(top_builddir)/src/utils/dllinit.o: $(top_srcdir)/src/utils/dllinit.c
	$(MAKE) -C $(top_builddir)/src/utils dllinit.o

# The postgres.o target is needed by the rule in Makefile.global that
# creates the exports file when MAKE_EXPORTS = true.
postgres.o: $(OBJS)
	$(CC) $(LDREL) $(LDOUT) $@ $^ $(LDFLAGS)


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
	cd $(dir $@) && rm -f $(notdir $@) && \
	    $(LN_S) ../../../$(subdir)/parser/parse.h .

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
ifeq ($(PORTNAME), win)
ifeq ($(MAKE_DLL), true)
	$(INSTALL_DATA) libpostgres.a $(DESTDIR)$(libdir)/libpostgres.a
endif
endif
	$(MAKE) -C catalog install-bki
	$(INSTALL_DATA) libpq/pg_hba.conf.sample $(DESTDIR)$(datadir)/pg_hba.conf.sample
	$(INSTALL_DATA) libpq/pg_ident.conf.sample $(DESTDIR)$(datadir)/pg_ident.conf.sample
	$(INSTALL_DATA) utils/misc/postgresql.conf.sample $(DESTDIR)$(datadir)/postgresql.conf.sample

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(libdir) $(DESTDIR)$(datadir)

install-bin: postgres $(POSTGRES_IMP) installdirs
	$(INSTALL_PROGRAM) postgres$(X) $(DESTDIR)$(bindir)/postgres$(X)
	@rm -f $(DESTDIR)$(bindir)/postmaster
	ln -s postgres$(X) $(DESTDIR)$(bindir)/postmaster
ifeq ($(MAKE_EXPORTS), true)
	$(INSTALL_DATA) $(POSTGRES_IMP) $(DESTDIR)$(libdir)/$(POSTGRES_IMP)
endif

.PHONY: install-bin

##########################################################################

uninstall:
	rm -f $(DESTDIR)$(bindir)/postgres$(X) $(DESTDIR)$(bindir)/postmaster
ifeq ($(MAKE_EXPORTS), true)
	rm -f $(DESTDIR)$(libdir)/$(POSTGRES_IMP)
endif
ifeq ($(PORTNAME), win)
ifeq ($(MAKE_DLL), true)
	rm -f $(DESTDIR)$(libdir)/libpostgres.a
endif
endif
	$(MAKE) -C catalog uninstall-bki
	rm -f $(DESTDIR)$(datadir)/pg_hba.conf.sample \
	      $(DESTDIR)$(datadir)/pg_ident.conf.sample \
	      $(DESTDIR)$(datadir)/postgresql.conf.sample


##########################################################################

clean:
	rm -f postgres$(X) $(POSTGRES_IMP) \
		$(top_srcdir)/src/include/parser/parse.h \
		$(top_builddir)/src/include/utils/fmgroids.h
ifeq ($(PORTNAME), win)
ifeq ($(MAKE_DLL), true)
	rm -f postgres.dll postgres.def libpostgres.a
endif
endif
	for i in $(DIRS); do $(MAKE) -C $$i clean || exit; done

distclean: clean
	rm -f port/Makefile port/tas.s port/dynloader.c

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
	$(CC) -o postgres $(OBJS) $(LDFLAGS)

depend dep: $(top_srcdir)/src/include/parser/parse.h $(top_builddir)/src/include/utils/fmgroids.h
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
