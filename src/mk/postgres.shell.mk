#-------------------------------------------------------------------------
#
# postgres.shell.mk--
#    rules for building shell scripts. To use the rules, set the following
#    variables:
#	SRCS    - source for the shell script
#	SHPROG	- name of the executable
#    postgres.mk should be included before this file.
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/mk/Attic/postgres.shell.mk,v 1.3 1996/09/24 01:57:01 scrappy Exp $
#
# NOTES
#    the shell script you write might include the following strings which
#    will be turned into the values listed below:
#
#	_fUnKy_BINDIR_sTuFf_	  -  location of installed binaries
#	_fUnKy_LIBDIR_sTuFf_	  -  location of installed library stuff
#	_fUnKy_DATADIR_sTuFf_	  -  location of the default data directory
#	_fUnKy_POSTGRESDIR_sTuFf_ -  location of the postgres "home" directory
#	_fUnKy_POSTPORT_sTuFf_    -  port to run the postmaster on
#	_fUnKy_NAMEDATALEN_sTuFf_ -  length of a datum of type "name"
#	_fUnKy_OIDNAMELEN_sTuFf_  -  ?
#	_fUnKy_IPCCLEANPATH_sTuFf_ - location of the ipcs and ipcrm programs
#	_fUnKy_DASH_N_sTuFf_	  -  -n flag used in echo
#	_fUnKy_BACKSLASH_C_sTuFf_ -  continuation (echo)
#-------------------------------------------------------------------------

#
# Insert installation-dependent filepaths into the shell script
#
SEDSCRIPT= \
    -e "s^_fUnKy_BINDIR_sTuFf_^$(BINDIR)^g" \
    -e "s^_fUnKy_LIBDIR_sTuFf_^$(LIBDIR)^g" \
    -e "s^_fUnKy_DATADIR_sTuFf_^$(DATADIR)^g" \
    -e "s^_fUnKy_IPCCLEANPATH_sTuFf_^$(IPCSDIR)^g" \
    -e "s^_fUnKy_NAMEDATALEN_sTuFf_^$(NAMEDATALEN)^g" \
    -e "s^_fUnKy_OIDNAMELEN_sTuFf_^$(OIDNAMELEN)^g" \
    -e "s^_fUnKy_POSTPORT_sTuFf_^$(POSTPORT)^g"

#
# We also need to fix up the scripts to deal with the lack of installed
# 'echo' commands that accept the -n option.
#
ifndef DASH_N
DASH_N=-n
endif
ifndef BACKSLASH_C
BACKSLASH_C=
endif

SEDSCRIPT+= -e "s^_fUnKy_DASH_N_sTuFf_^$(DASH_N)^g" \
	-e "s^_fUnKy_BACKSLASH_C_sTuFf_^$(BACKSLASH_C)^g"


OBJS:= $(SRCS:%.c=%.o)

$(SHPROG):  $(SHPROG).sh
	sed $(SEDSCRIPT) < $< > $(objdir)/$(SHPROG)

CLEANFILES+= $(SHPROG)

install:	localobj $(SHPROG)
	$(INSTALL) $(INSTL_EXE_OPTS) $(objdir)/$(SHPROG) $(DESTDIR)$(BINDIR)/$(SHPROG)


