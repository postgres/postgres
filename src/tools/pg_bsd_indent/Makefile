#-------------------------------------------------------------------------
#
# src/tools/pg_bsd_indent/Makefile
#
# Copyright (c) 2017-2024, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_bsd_indent - indent C code nicely"
PGAPPICON = win32

subdir = src/tools/pg_bsd_indent
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS = \
	$(WIN32RES) \
	args.o \
	err.o \
	indent.o \
	io.o \
	lexi.o \
	parse.o \
	pr_comment.o

$(OBJS): CFLAGS += $(PERMIT_MISSING_VARIABLE_DECLARATIONS)

all: pg_bsd_indent

pg_bsd_indent: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_bsd_indent$(X) '$(DESTDIR)$(bindir)/pg_bsd_indent$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_bsd_indent$(X)'

clean distclean:
	rm -f pg_bsd_indent$(X) $(OBJS)
	rm -rf log/ tmp_check/

check: pg_bsd_indent
	$(prove_check)

installcheck:
	$(prove_installcheck)

# Provide this alternate test name to allow testing pg_bsd_indent
# without building all of the surrounding Postgres installation.
.PHONY: test

test: pg_bsd_indent
	$(prove_installcheck)
