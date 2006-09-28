#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/Makefile,v 1.44 2006/09/28 14:48:45 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/ecpglib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg
SO_MAJOR_VERSION= 5
SO_MINOR_VERSION= 2
DLTYPE= library

override CPPFLAGS := -DFRONTEND \
	-I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_builddir)/src/port $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

# Need to recompile any libpgport object files
LIBS := $(filter-out -lpgport, $(LIBS))

OBJS= execute.o typename.o descriptor.o data.o error.o prepare.o memory.o \
	connect.o misc.o path.o \
	$(filter snprintf.o strlcpy.o, $(LIBOBJS))

# thread.c is needed only for non-WIN32 implementation of path.c
ifneq ($(PORTNAME), win32)
OBJS += thread.o
endif

SHLIB_LINK = -L../pgtypeslib -lpgtypes $(libpq) -lm $(PTHREAD_LIBS)

ifeq ($(PORTNAME), win32)
# Link to shfolder.dll instead of shell32.dll
SHLIB_LINK += -lshfolder
endif

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# We use some port modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.

path.c snprintf.c strlcpy.c thread.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

path.o: path.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(libdir)'

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) path.c snprintf.c strlcpy.c thread.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
