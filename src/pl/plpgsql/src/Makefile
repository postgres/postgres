#-------------------------------------------------------------------------
#
# Makefile for the plpgsql shared object
#
# $PostgreSQL: pgsql/src/pl/plpgsql/src/Makefile,v 1.32 2008/04/07 14:15:58 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME= plpgsql

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
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
pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o: plpgsql.h $(srcdir)/pl.tab.h

# pl_scan is compiled as part of pl_gram
pl_gram.o: $(srcdir)/pl_scan.c

# See notes in src/backend/parser/Makefile about the following two rules

$(srcdir)/pl_gram.c: $(srcdir)/pl.tab.h ;

$(srcdir)/pl.tab.h: gram.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c $(srcdir)/pl_gram.c
	mv -f y.tab.h $(srcdir)/pl.tab.h
else
	@$(missing) bison $< $@
endif

# Because we use %option case-insensitive, flex's results could vary
# depending on what the compile-time locale setting is.  Hence, force
# it to see LC_CTYPE=C to ensure consistent build results.

$(srcdir)/pl_scan.c: scan.l
ifdef FLEX
	LC_CTYPE=C $(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/pl_scan.c $(srcdir)/pl.tab.h $(srcdir)/pl_gram.c

# pl_gram.c, pl.tab.h, and pl_scan.c are in the distribution tarball,
# so they are not cleaned here.
clean distclean: clean-lib
	rm -f $(OBJS)
# And the garbage that might have been left behind by partial build:
	@rm -f y.tab.h y.tab.c y.output lex.yy.c

maintainer-clean: clean
	rm -f $(srcdir)/pl_gram.c $(srcdir)/pl.tab.h $(srcdir)/pl_scan.c
