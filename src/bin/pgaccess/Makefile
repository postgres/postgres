#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for pgaccess
#
# Copyright (c) 1994, Regents of the University of California
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.7 1999/10/31 19:50:47 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include $(SRCDIR)/Makefile.global

all: pgaccess

pgaccess: pgaccess.sh
	sed -e 's;__wish__;$(WISH);' pgaccess.sh |	\
	sed -e 's;__POSTGRESDIR__;$(POSTGRESDIR);' 	\
			> pgaccess

install:
	$(INSTALL) $(INSTL_EXE_OPTS) pgaccess $(BINDIR)/pgaccess
	@if [ ! -d $(POSTGRESDIR)/pgaccess ]; then mkdir $(POSTGRESDIR)/pgaccess; fi
	$(INSTALL) $(INSTL_EXE_OPTS) main.tcl $(POSTGRESDIR)/pgaccess
	@if [ ! -d $(POSTGRESDIR)/pgaccess/lib ]; then mkdir $(POSTGRESDIR)/pgaccess/lib; fi
	$(INSTALL) $(INSTLOPTS) lib/*.tcl $(POSTGRESDIR)/pgaccess/lib
	@if [ ! -d $(POSTGRESDIR)/pgaccess/lib/help ]; then mkdir $(POSTGRESDIR)/pgaccess/lib/help; fi
	$(INSTALL) $(INSTLOPTS) lib/help/*.hlp $(POSTGRESDIR)/pgaccess/lib/help
	@if [ ! -d $(POSTGRESDIR)/pgaccess/lib/languages ]; then mkdir $(POSTGRESDIR)/pgaccess/lib/languages; fi
	$(INSTALL) $(INSTLOPTS) lib/languages/[a-z]* $(POSTGRESDIR)/pgaccess/lib/languages
	@if [ ! -d $(POSTGRESDIR)/pgaccess/images ]; then mkdir $(POSTGRESDIR)/pgaccess/images; fi
	$(INSTALL) $(INSTLOPTS) images/*.gif $(POSTGRESDIR)/pgaccess/images

clean:
	rm -f pgaccess
