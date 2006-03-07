#-------------------------------------------------------------------------
#
# Makefile for the plpgsql shared object
#
# $PostgreSQL: pgsql/src/pl/plpgsql/src/Makefile,v 1.29 2006/03/07 01:03:12 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME= plpgsql
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
SHLIB_LINK = $(filter -lintl, $(LIBS)) $(BE_DLLLIBS)
rpath :=

OBJS = pl_gram.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


# In order to use Makefile.shlib, we allow it to build a static
# library libplpgsql.a, which we just ignore, as well as a shared
# library that it will insist on naming $(shlib). We don't want to
# call it that when installed, however, so we ignore the install-shlib
# rule and do this instead:

install: installdirs all
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(pkglibdir)/plpgsql$(DLSUFFIX)'
else
	@echo "*****"; \
	 echo "* PL/pgSQL was not installed due to lack of shared library support."; \
	 echo "*****"
endif

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)'

uninstall:
	rm -f '$(DESTDIR)$(pkglibdir)/plpgsql$(DLSUFFIX)'

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

$(srcdir)/pl_scan.c: scan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
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
