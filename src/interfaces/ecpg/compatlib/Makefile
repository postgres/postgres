#-------------------------------------------------------------------------
#
# Makefile for ecpg compatibility library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/compatlib/Makefile,v 1.28 2007/10/03 13:20:19 meskes Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/compatlib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg_compat
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 0
DLTYPE= library

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_srcdir)/src/include/utils $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)
SHLIB_LINK = -L../ecpglib -lecpg -L../pgtypeslib -lpgtypes $(libpq) -lm \
	$(PTHREAD_LIBS)

OBJS= informix.o $(filter snprintf.o, $(LIBOBJS))

ifneq ($(PORTNAME), win32)
DLL_DEFFILE=libecpg_compatdll.def
endif

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# We need several not-quite-identical variants of .DEF files to build libecpg
# DLLs for Windows.  These are made from the single source file exports.txt.
# Since we can't assume that Windows boxes will have sed, the .DEF files are
# always built and included in distribution tarballs.

.PHONY: def-files
def-files: $(srcdir)/libecpg_compat.def $(srcdir)/blibecpg_compatdll.def
$(srcdir)/libecpg_compat.def: exports.txt
	echo '; DEF file for MS VC++' > $@
	echo 'LIBRARY LIBECPG_COMPAT' >> $@
	echo 'EXPORTS' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    \1@ \2/' < $< >> $@

$(srcdir)/blibecpg_compatdll.def: exports.txt
	echo '; DEF file for Borland C++ Builder' > $@
	echo 'LIBRARY BLIBECPG_COMPAT' >> $@
	echo 'EXPORTS' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    _\1@ \2/' < $< >> $@
	echo '' >> $@
	echo '; Aliases for MS compatible names' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    \1= _\1/' < $< | sed 's/ *$$//' >> $@

# Where possible, restrict the symbols exported by the library to just the
# official list, so as to avoid unintentional ABI changes.

ifeq ($(PORTNAME), darwin)
$(shlib): exports.list

exports.list: exports.txt
	$(AWK) '/^[^#]/ {printf "_%s\n",$$1}' $< >$@

exported_symbols_list = -exported_symbols_list exports.list
endif

ifeq ($(PORTNAME), linux)
$(shlib): exports.list

exports.list: exports.txt
	echo '{ global:' >$@
	$(AWK) '/^[^#]/ {printf "%s;\n",$$1}' $< >>$@
	echo ' local: *; };' >>$@

exported_symbols_list = -Wl,--version-script=exports.list
endif

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(libdir)'

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) snprintf.c
