#-------------------------------------------------------------------------
#
# Makefile for libpq-oauth
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/libpq-oauth/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq-oauth
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "libpq-oauth - device authorization OAuth support"

# This is an internal module; we don't want an SONAME and therefore do not set
# SO_MAJOR_VERSION.
NAME = pq-oauth-$(MAJORVERSION)

# Force the name "libpq-oauth" for both the static and shared libraries. The
# staticlib doesn't need version information in its name.
override shlib := lib$(NAME)$(DLSUFFIX)
override stlib := libpq-oauth.a

override CPPFLAGS := -I$(libpq_srcdir) -I$(top_builddir)/src/port $(LIBCURL_CPPFLAGS) $(CPPFLAGS)

OBJS = \
	$(WIN32RES)

OBJS_STATIC = oauth-curl.o

# The shared library needs additional glue symbols.
OBJS_SHLIB = \
	oauth-curl_shlib.o \
	oauth-utils.o \

oauth-utils.o: override CPPFLAGS += -DUSE_DYNAMIC_OAUTH
oauth-curl_shlib.o: override CPPFLAGS_SHLIB += -DUSE_DYNAMIC_OAUTH

# Add shlib-/stlib-specific objects.
$(shlib): override OBJS += $(OBJS_SHLIB)
$(shlib): $(OBJS_SHLIB)

$(stlib): override OBJS += $(OBJS_STATIC)
$(stlib): $(OBJS_STATIC)

SHLIB_LINK_INTERNAL = $(libpq_pgport_shlib)
SHLIB_LINK = $(LIBCURL_LDFLAGS) $(LIBCURL_LDLIBS) $(filter -lintl, $(LIBS))
SHLIB_PREREQS = submake-libpq
SHLIB_EXPORTS = exports.txt

# Disable -bundle_loader on macOS.
BE_DLLLIBS =

# By default, a library without an SONAME doesn't get a static library, so we
# add it to the build explicitly.
all: all-lib all-static-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# Use src/common/Makefile's trick for tracking dependencies of shlib-specific
# objects.
%_shlib.o: %.c %.o
	$(CC) $(CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) $(CPPFLAGS_SHLIB) -c $< -o $@

# Ignore the standard rules for SONAME-less installation; we want both the
# static and shared libraries to go into libdir.
install: all installdirs $(stlib) $(shlib)
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(libdir)/$(shlib)'
	$(INSTALL_STLIB) $(stlib) '$(DESTDIR)$(libdir)/$(stlib)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/$(stlib)'
	rm -f '$(DESTDIR)$(libdir)/$(shlib)'

clean distclean: clean-lib
	rm -f $(OBJS) $(OBJS_STATIC) $(OBJS_SHLIB)
