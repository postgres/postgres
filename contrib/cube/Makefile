#
# $Header: /cvsroot/pgsql/contrib/cube/Makefile,v 1.3 2001/02/20 19:20:27 petere Exp $
#

subdir = contrib/cube
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# override libdir to install shlib in contrib not main directory
libdir := $(libdir)/contrib

# shared library parameters
NAME= cube
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS= cube.o cubeparse.o cubescan.o buffer.o

all: all-lib $(NAME).sql

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


cubeparse.c cubeparse.h: cubeparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) -p cube_yy $<
	mv -f y.tab.c cubeparse.c
	mv -f y.tab.h cubeparse.h
else
	@$(missing) bison $< $@
endif

cubescan.c: cubescan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -Pcube_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/$(shlib):g' < $< > $@

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress

# against installed postmaster
installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress cube

# in-tree test doesn't work yet (no way to install my shared library)
#check: all submake
#	$(top_builddir)/src/test/regress/pg_regress --temp-install \
#	  --top-builddir=$(top_builddir) seg
check:
	@echo "'make check' is not supported."
	@echo "Do 'make install', then 'make installcheck' instead."

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/README.$(NAME)  $(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql $(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.$(NAME) $(datadir)/contrib/$(NAME).sql

clean distclean maintainer-clean: clean-lib
	rm -f cubeparse.c cubeparse.h cubescan.c
	rm -f y.tab.c y.tab.h $(OBJS) $(NAME).sql
# things created by various check targets
	rm -rf results tmp_check log
	rm -f regression.diffs regression.out regress.out run_check.out
ifeq ($(PORTNAME), win)
	rm -f regress.def
endif

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
