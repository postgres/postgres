#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/Makefile,v 1.66 2010/01/05 16:38:23 meskes Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/ecpglib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg
SO_MAJOR_VERSION= 6
SO_MINOR_VERSION= 2

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

# Need to recompile any libpgport object files
LIBS := $(filter-out -lpgport, $(LIBS))

OBJS= execute.o typename.o descriptor.o sqlda.o data.o error.o prepare.o memory.o \
	connect.o misc.o path.o pgstrcasecmp.o \
	$(filter snprintf.o strlcpy.o isinf.o, $(LIBOBJS))

# thread.c is needed only for non-WIN32 implementation of path.c
ifneq ($(PORTNAME), win32)
OBJS += thread.o
endif

SHLIB_LINK = -L../pgtypeslib -lpgtypes $(libpq) $(filter -lintl -lm, $(LIBS)) $(PTHREAD_LIBS)

SHLIB_EXPORTS = exports.txt

ifeq ($(PORTNAME), win32)
# Link to shfolder.dll instead of shell32.dll
SHLIB_LINK += -lshfolder
endif

all: libpq pgtypeslib all-lib

libpq:
	$(MAKE) -C $(top_builddir)/src/interfaces/libpq all

pgtypeslib:
	$(MAKE) -C $(top_builddir)/src/interfaces/ecpg/pgtypeslib all

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# We use some port modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.

path.c pgstrcasecmp.c snprintf.c strlcpy.c thread.c isinf.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

misc.o: misc.c $(top_builddir)/src/port/pg_config_paths.h
path.o: path.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)
	rm -f path.c pgstrcasecmp.c snprintf.c strlcpy.c thread.c

maintainer-clean: distclean maintainer-clean-lib
