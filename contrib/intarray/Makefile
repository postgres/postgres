subdir = contrib/intarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# override libdir to install shlib in contrib not main directory
libdir := $(libdir)/contrib

# shared library parameters
NAME= _int
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0

override CPPFLAGS += -I$(srcdir) -DPGSQL71

OBJS= _int.o

all: all-lib $(NAME).sql

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/$(shlib):g' < $< > $@

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress

# against installed postmaster
installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress _int

# in-tree test doesn't work yet (no way to install my shared library)
#check: all submake
#	$(top_builddir)/src/test/regress/pg_regress --temp-install \
#	  --top-builddir=$(top_builddir) _int
check:
	@echo "'make check' is not supported."
	@echo "Do 'make install', then 'make installcheck' instead."

install: all installdirs install-lib
	#$(INSTALL_DATA) $(srcdir)/README.$(NAME)  $(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql $(datadir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)

uninstall: uninstall-lib
	rm -f $(docdir)/contrib/README.$(NAME) $(datadir)/contrib/$(NAME).sql

clean distclean maintainer-clean: clean-lib
	rm -f *.so y.tab.c y.tab.h $(OBJS) $(NAME).sql
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
