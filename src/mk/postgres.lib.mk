#-------------------------------------------------------------------------
#
# postgres.lib.mk--
#    rules for building libraries. To use the rules, set the following
#    variables:
#	LIBSRCS    - source files for objects to be built in the library
#	LIB	   - name of the library (eg. LIB=pq for libpq.a)
#    postgres.mk should be included before this file.
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/mk/Attic/postgres.lib.mk,v 1.2 1996/08/13 07:48:29 scrappy Exp $
#
#-------------------------------------------------------------------------

LIBOBJS:=     $(addsuffix .o, $(basename $(LIBSRCS)))
#LIBSOBJS:=    $(addsuffix .so, $(basename $(LIBSRCS)))
lib:=		lib$(LIB).a
shlib:=		lib$(LIB).so.1

ifndef LINUX_ELF
$(lib):	$(addprefix $(objdir)/,$(LIBOBJS))
else
$(lib):	$(addprefix $(objdir)/,$(LIBOBJS))
endif
	@rm -f $(objdir)/$(lib)
ifdef MK_NO_LORDER
	cd $(objdir); $(AR) $(AROPT) $(lib) $(LIBOBJS); $(RANLIB) $(lib)
else
	cd $(objdir); $(AR) $(AROPT) $(lib) `lorder $(LIBOBJS) | tsort`; $(RANLIB) $(lib)
endif

$(shlib):	$(addprefix $(objdir)/,$(LIBOBJS))
	@rm -f $(objdir)/$(shlib)
	cd $(objdir); $(CC) $(LDFLAGS) -shared $(LIBOBJS) -o $(shlib) 

CLEANFILES+= $(LIBOBJS) $(lib) $(shlib)

ifdef LINUX_ELF
install:: localobj $(lib) $(shlib)
	$(INSTALL) $(INSTL_LIB_OPTS) $(objdir)/$(lib) $(DESTDIR)$(LIBDIR)/$(lib)
	$(INSTALL) $(INSTL_LIB_OPTS) $(objdir)/$(shlib) $(DESTDIR)$(LIBDIR)/$(shlib)
else
install:: localobj $(lib)
	$(INSTALL) $(INSTL_LIB_OPTS) $(objdir)/$(lib) $(DESTDIR)$(LIBDIR)/$(lib)
endif
#	@cd $(DESTDIR)$(LIBDIR); $(RANLIB) $(lib)


