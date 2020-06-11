#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/common
#
# These files are used by the Postgres backend, and also by frontend
# programs.  These files provide common functionality that isn't directly
# concerned with portability and thus doesn't belong in src/port.
#
# This makefile generates three outputs:
#
#	libpgcommon.a - contains object files with FRONTEND defined,
#		for use by client applications
#
#	libpgcommon_shlib.a - contains object files with FRONTEND defined,
#		built suitably for use in shared libraries; for use
#		by frontend libraries
#
#	libpgcommon_srv.a - contains object files without FRONTEND defined,
#		for use only by the backend
#
# IDENTIFICATION
#    src/common/Makefile
#
#-------------------------------------------------------------------------

subdir = src/common
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# don't include subdirectory-path-dependent -I and -L switches
STD_CPPFLAGS := $(filter-out -I$(top_srcdir)/src/include -I$(top_builddir)/src/include,$(CPPFLAGS))
STD_LDFLAGS := $(filter-out -L$(top_builddir)/src/common -L$(top_builddir)/src/port,$(LDFLAGS))
override CPPFLAGS += -DVAL_CC="\"$(CC)\""
override CPPFLAGS += -DVAL_CPPFLAGS="\"$(STD_CPPFLAGS)\""
override CPPFLAGS += -DVAL_CFLAGS="\"$(CFLAGS)\""
override CPPFLAGS += -DVAL_CFLAGS_SL="\"$(CFLAGS_SL)\""
override CPPFLAGS += -DVAL_LDFLAGS="\"$(STD_LDFLAGS)\""
override CPPFLAGS += -DVAL_LDFLAGS_EX="\"$(LDFLAGS_EX)\""
override CPPFLAGS += -DVAL_LDFLAGS_SL="\"$(LDFLAGS_SL)\""
override CPPFLAGS += -DVAL_LIBS="\"$(LIBS)\""

override CPPFLAGS := -DFRONTEND -I. -I$(top_srcdir)/src/common $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

# If you add objects here, see also src/tools/msvc/Mkvcbuild.pm

OBJS_COMMON = \
	archive.o \
	base64.o \
	checksum_helper.o \
	config_info.o \
	controldata_utils.o \
	d2s.o \
	encnames.o \
	exec.o \
	f2s.o \
	file_perm.o \
	hashfn.o \
	ip.o \
	jsonapi.o \
	keywords.o \
	kwlookup.o \
	link-canary.o \
	md5.o \
	pg_lzcompress.o \
	pgfnames.o \
	psprintf.o \
	relpath.o \
	rmtree.o \
	saslprep.o \
	scram-common.o \
	string.o \
	stringinfo.o \
	unicode_norm.o \
	username.o \
	wait_error.o \
	wchar.o

ifeq ($(with_openssl),yes)
OBJS_COMMON += \
	protocol_openssl.o \
	sha2_openssl.o
else
OBJS_COMMON += sha2.o
endif

# A few files are currently only built for frontend, not server
# (Mkvcbuild.pm has a copy of this list, too)
OBJS_FRONTEND = \
	$(OBJS_COMMON) \
	fe_memutils.o \
	file_utils.o \
	logging.o \
	restricted_token.o

# foo.o, foo_shlib.o, and foo_srv.o are all built from foo.c
OBJS_SHLIB = $(OBJS_FRONTEND:%.o=%_shlib.o)
OBJS_SRV = $(OBJS_COMMON:%.o=%_srv.o)

# where to find gen_keywordlist.pl and subsidiary files
TOOLSDIR = $(top_srcdir)/src/tools
GEN_KEYWORDLIST = $(PERL) -I $(TOOLSDIR) $(TOOLSDIR)/gen_keywordlist.pl
GEN_KEYWORDLIST_DEPS = $(TOOLSDIR)/gen_keywordlist.pl $(TOOLSDIR)/PerfectHash.pm

all: libpgcommon.a libpgcommon_shlib.a libpgcommon_srv.a

distprep: kwlist_d.h

# libpgcommon is needed by some contrib
install: all installdirs
	$(INSTALL_STLIB) libpgcommon.a '$(DESTDIR)$(libdir)/libpgcommon.a'
	$(INSTALL_STLIB) libpgcommon_shlib.a '$(DESTDIR)$(libdir)/libpgcommon_shlib.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgcommon.a'
	rm -f '$(DESTDIR)$(libdir)/libpgcommon_shlib.a'

libpgcommon.a: $(OBJS_FRONTEND)
	rm -f $@
	$(AR) $(AROPT) $@ $^

#
# Shared library versions of object files
#

libpgcommon_shlib.a: $(OBJS_SHLIB)
	rm -f $@
	$(AR) $(AROPT) $@ $^

# Because this uses its own compilation rule, it doesn't use the
# dependency tracking logic from Makefile.global.  To make sure that
# dependency tracking works anyway for the *_shlib.o files, depend on
# their *.o siblings as well, which do have proper dependencies.  It's
# a hack that might fail someday if there is a *_shlib.o without a
# corresponding *.o, but there seems little reason for that.
%_shlib.o: %.c %.o
	$(CC) $(CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) -c $< -o $@

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
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

# generate SQL keyword lookup table to be included into keywords*.o.
kwlist_d.h: $(top_srcdir)/src/include/parser/kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --extern $<

# Dependencies of keywords*.o need to be managed explicitly to make sure
# that you don't get broken parsing code, even in a non-enable-depend build.
keywords.o keywords_shlib.o keywords_srv.o: kwlist_d.h

# The code imported from Ryu gets a pass on declaration-after-statement,
# in order to keep it more closely aligned with its upstream.
RYU_FILES = d2s.o f2s.o
RYU_OBJS = $(RYU_FILES) $(RYU_FILES:%.o=%_shlib.o) $(RYU_FILES:%.o=%_srv.o)

$(RYU_OBJS): CFLAGS += $(PERMIT_DECLARATION_AFTER_STATEMENT)

# kwlist_d.h is in the distribution tarball, so it is not cleaned here.
clean distclean:
	rm -f libpgcommon.a libpgcommon_shlib.a libpgcommon_srv.a
	rm -f $(OBJS_FRONTEND) $(OBJS_SHLIB) $(OBJS_SRV)

maintainer-clean: distclean
	rm -f kwlist_d.h
