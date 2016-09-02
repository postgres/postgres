#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/common
#
# This makefile generates two outputs:
#
#	libpgcommon.a - contains object files with FRONTEND defined,
#		for use by client applications
#
#	libpgcommon_srv.a - contains object files without FRONTEND defined,
#		for use only by the backend binaries
#
# You can also symlink/copy individual source files from this directory,
# to compile with different options. (libpq does that, because it needs
# to use -fPIC on some platforms.)
#
# IDENTIFICATION
#    src/common/Makefile
#
#-------------------------------------------------------------------------

subdir = src/common
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

# don't include subdirectory-path-dependent -I and -L switches
STD_CPPFLAGS := $(filter-out -I$(top_srcdir)/src/include -I$(top_builddir)/src/include,$(CPPFLAGS))
STD_LDFLAGS := $(filter-out -L$(top_builddir)/src/port,$(LDFLAGS))
override CPPFLAGS += -DVAL_CONFIGURE="\"$(configure_args)\""
override CPPFLAGS += -DVAL_CC="\"$(CC)\""
override CPPFLAGS += -DVAL_CPPFLAGS="\"$(STD_CPPFLAGS)\""
override CPPFLAGS += -DVAL_CFLAGS="\"$(CFLAGS)\""
override CPPFLAGS += -DVAL_CFLAGS_SL="\"$(CFLAGS_SL)\""
override CPPFLAGS += -DVAL_LDFLAGS="\"$(STD_LDFLAGS)\""
override CPPFLAGS += -DVAL_LDFLAGS_EX="\"$(LDFLAGS_EX)\""
override CPPFLAGS += -DVAL_LDFLAGS_SL="\"$(LDFLAGS_SL)\""
override CPPFLAGS += -DVAL_LIBS="\"$(LIBS)\""

OBJS_COMMON = config_info.o controldata_utils.o exec.o ip.o keywords.o \
	md5.o pg_lzcompress.o pgfnames.o psprintf.o relpath.o rmtree.o \
	string.o username.o wait_error.o

OBJS_FRONTEND = $(OBJS_COMMON) fe_memutils.o restricted_token.o

OBJS_SRV = $(OBJS_COMMON:%.o=%_srv.o)

all: libpgcommon.a libpgcommon_srv.a

# libpgcommon is needed by some contrib
install: all installdirs
	$(INSTALL_STLIB) libpgcommon.a '$(DESTDIR)$(libdir)/libpgcommon.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgcommon.a'

libpgcommon.a: $(OBJS_FRONTEND)
	rm -f $@
	$(AR) $(AROPT) $@ $^

#
# Server versions of object files
#

libpgcommon_srv.a: $(OBJS_SRV)
	rm -f $@
	$(AR) $(AROPT) $@ $^

# Because this uses its own compilation rule, it doesn't use the
# dependency tracking logic from Makefile.global.  To make sure that
# dependency tracking works anyway for the *_srv.o files, depend on
# their *.o siblings as well, which do have proper dependencies.  It's
# a hack that might fail someday if there is a *_srv.o without a
# corresponding *.o, but it works for now.
%_srv.o: %.c %.o
	$(CC) $(CFLAGS) $(subst -DFRONTEND ,, $(CPPFLAGS)) -c $< -o $@

$(OBJS_SRV): | submake-errcodes

.PHONY: submake-errcodes

submake-errcodes:
	$(MAKE) -C ../backend submake-errcodes

# Dependencies of keywords.o need to be managed explicitly to make sure
# that you don't get broken parsing code, even in a non-enable-depend build.
# Note that gram.h isn't required for the frontend version of keywords.o.
$(top_builddir)/src/include/parser/gram.h: $(top_srcdir)/src/backend/parser/gram.y
	$(MAKE) -C $(top_builddir)/src/backend $(top_builddir)/src/include/parser/gram.h

keywords.o: $(top_srcdir)/src/include/parser/kwlist.h
keywords_srv.o: $(top_builddir)/src/include/parser/gram.h $(top_srcdir)/src/include/parser/kwlist.h

clean distclean maintainer-clean:
	rm -f libpgcommon.a libpgcommon_srv.a $(OBJS_FRONTEND) $(OBJS_SRV)
