#-------------------------------------------------------------------------
#
# Makefile for the plpgsql shared object
#
# $Header: /cvsroot/pgsql/src/pl/plpgsql/src/Makefile,v 1.3 2000/06/27 00:32:01 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql/src
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME= plpgsql
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

CPPFLAGS += -I$(srcdir)

OBJS = pl_parse.o pl_handler.o pl_comp.o pl_exec.o pl_funcs.o

# Shared library stuff, also default `all' target
include $(top_srcdir)/src/Makefile.shlib


# In order to use Makefile.shlib, we allow it to build a static
# library libplpgsql.a, which we just ignore, as well as a shared
# library that it will insist on naming $(shlib). We don't want to
# call it that when installed, however, so we ignore the install-shlib
# rule and do this instead:

install: installdirs all
ifdef shlib
	$(INSTALL_SHLIB) $(shlib) $(libdir)/plpgsql$(DLSUFFIX)
else
	@echo "*****"; \
	 echo "* PL/pgSQL was not installed due to lack of shared library support."; \
	 echo "*****"
endif

installdirs:
	$(mkinstalldirs) $(libdir)

uninstall:
	rm -f $(libdir)/plpgsql$(DLSUFFIX)

pl_handler.o pl_comp.o pl_exec.o pl_funcs.o: plpgsql.h $(srcdir)/pl.tab.h

pl_parse.o: $(srcdir)/pl_gram.c $(srcdir)/pl_scan.c plpgsql.h
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

# Note: Since the yacc and lex files are shipped in the distribution,
# they must be generated in the srcdir (as opposed to builddir).

$(srcdir)/pl_gram.c $(srcdir)/pl.tab.h: gram.y
	$(YACC) -d $(YFLAGS) $<
	sed -e 's/yy/plpgsql_yy/g' -e 's/YY/PLPGSQL_YY/g' < y.tab.c > $(srcdir)/pl_gram.c
	sed -e 's/yy/plpgsql_yy/g' -e 's/YY/PLPGSQL_YY/g' < y.tab.h > $(srcdir)/pl.tab.h
	rm -f y.tab.c y.tab.h

# Assuming flex here for -i and -l options, since scan.l requires flex anyway.
$(srcdir)/pl_scan.c: scan.l
	$(LEX) $(LFLAGS) -i -l $<
	sed -e 's/yy/plpgsql_yy/g' -e 's/YY/PLPGSQL_YY/g' < lex.yy.c > $@
	rm -f lex.yy.c

mklang.sql: mklang.sql.in
	sed -e 's%__libdir__%$(libdir)%g' -e 's%__DLSUFFIX__%$(DLSUFFIX)%g' < $< > $@


clean distclean: clean-shlib
	rm -f lib$(NAME).a *.o y.tab.c y.tab.h lex.yy.c mklang.sql

maintainer-clean: clean
	rm -f $(srcdir)/pl_gram.c $(srcdir)/pl.tab.h $(srcdir)/pl_scan.c
