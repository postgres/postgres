#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/ecpg/preproc
#
# Copyright (c) 1998-2024, PostgreSQL Global Development Group
#
# src/interfaces/ecpg/preproc/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "ecpg - embedded SQL precompiler for C"
PGAPPICON=win32

subdir = src/interfaces/ecpg/preproc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I. -I$(srcdir) \
	-I$(top_srcdir)/src/interfaces/ecpg/ecpglib \
	-I$(libpq_srcdir) \
	$(CPPFLAGS)

override CFLAGS += $(PTHREAD_CFLAGS)

OBJS = \
	$(WIN32RES) \
	c_keywords.o \
	descriptor.o \
	ecpg.o \
	ecpg_keywords.o \
	keywords.o \
	output.o \
	parser.o \
	pgc.o \
	preproc.o \
	type.o \
	typename.o \
	util.o \
	variable.o

# where to find gen_keywordlist.pl and subsidiary files
TOOLSDIR = $(top_srcdir)/src/tools
GEN_KEYWORDLIST = $(PERL) -I $(TOOLSDIR) $(TOOLSDIR)/gen_keywordlist.pl
GEN_KEYWORDLIST_DEPS = $(TOOLSDIR)/gen_keywordlist.pl $(TOOLSDIR)/PerfectHash.pm

# Suppress parallel build to avoid a bug in GNU make 3.82
# (see comments in ../Makefile)
ifeq ($(MAKE_VERSION),3.82)
.NOTPARALLEL:
endif

all: ecpg

ecpg: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) $(PTHREAD_LIBS) -o $@$(X)

# We symlink typename.c from ecpglib and recompile it here
typename.c: % : $(top_srcdir)/src/interfaces/ecpg/ecpglib/%
	rm -f $@ && $(LN_S) $< .

# See notes in src/backend/parser/Makefile about the following two rules
preproc.h: preproc.c
	touch $@

preproc.c: BISONFLAGS += -d

preproc.y: ../../../backend/parser/gram.y parse.pl ecpg.addons ecpg.header ecpg.tokens ecpg.trailer ecpg.type
	$(PERL) $(srcdir)/parse.pl --srcdir $(srcdir) --parser $< --output $@

# generate keyword headers
c_kwlist_d.h: c_kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --varname ScanCKeywords --no-case-fold $<

ecpg_kwlist_d.h: ecpg_kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --varname ScanECPGKeywords $<

# Force these dependencies to be known even without dependency info built:
ecpg_keywords.o c_keywords.o keywords.o preproc.o pgc.o parser.o: preproc.h
ecpg_keywords.o: ecpg_kwlist_d.h
c_keywords.o: c_kwlist_d.h
keywords.o: $(top_srcdir)/src/include/parser/kwlist.h

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) '$(DESTDIR)$(bindir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ecpg$(X)'

clean distclean:
	rm -f *.o ecpg$(X)
	rm -f typename.c
	rm -f preproc.y preproc.c preproc.h pgc.c c_kwlist_d.h ecpg_kwlist_d.h
