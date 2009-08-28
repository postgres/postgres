#-------------------------------------------------------------------------
#
# Makefile for the plpgsql shared object
#
# $PostgreSQL: pgsql/src/pl/plpgsql/src/Makefile,v 1.34 2009/08/28 20:26:19 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME= plpgsql

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)
SHLIB_LINK = $(filter -lintl, $(LIBS))
rpath =

OBJS = pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


install: installdirs all install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib


# Force these dependencies to be known even without dependency info built:
pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o: plpgsql.h pl_gram.h

# pl_scan is compiled as part of pl_gram
pl_gram.o: pl_scan.c

# See notes in src/backend/parser/Makefile about the following two rules

pl_gram.h: pl_gram.c ;

pl_gram.c: gram.y
ifdef BISON
	$(BISON) -d $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

# Because we use %option case-insensitive, flex's results could vary
# depending on what the compile-time locale setting is.  Hence, force
# it to see LC_CTYPE=C to ensure consistent build results.

pl_scan.c: scan.l
ifdef FLEX
	LC_CTYPE=C $(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: pl_scan.c pl_gram.h pl_gram.c

# pl_gram.c, pl_gram.h, and pl_scan.c are in the distribution tarball,
# so they are not cleaned here.
clean distclean: clean-lib
	rm -f $(OBJS)

maintainer-clean: clean
	rm -f pl_gram.c pl_gram.h pl_scan.c
