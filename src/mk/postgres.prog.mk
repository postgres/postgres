#-------------------------------------------------------------------------
#
# postgres.prog.mk--
#    rules for building binaries. To use the rules, set the following
#    variables:
#	PROG	   - name of the program (eg. PROG=monitor for monitor)
#    postgres.mk should be included before this file.
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/mk/Attic/postgres.prog.mk,v 1.2 1996/08/13 07:48:33 scrappy Exp $
#
#-------------------------------------------------------------------------

PROGOBJS:= $(SRCS:%.c=%.o)

$(PROG):  $(addprefix $(objdir)/,$(PROGOBJS))
	$(CC) $(LDFLAGS) -o $(objdir)/$(@F) $(addprefix $(objdir)/,$(PROGOBJS)) $(LD_ADD)

CLEANFILES+= $(PROGOBJS) $(PROG)

install::	localobj $(PROG)
	$(INSTALL) $(INSTL_EXE_OPTS) $(objdir)/$(PROG) $(DESTDIR)$(BINDIR)/$(PROG)

