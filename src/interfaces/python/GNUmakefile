# $Header: /cvsroot/pgsql/src/interfaces/python/Attic/GNUmakefile,v 1.12 2001/12/03 12:39:44 darcy Exp $

subdir = src/interfaces/python
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

NAME = _pgmodule
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS = pgmodule.o
SHLIB_LINK = $(libpq)
ifeq ($(PORTNAME), win)
override CPPFLAGS += -DUSE_DL_IMPORT
SHLIB_LINK += $(python_libspec)
endif


include $(top_srcdir)/src/Makefile.shlib

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) $(python_includespec)

PY_SCRIPTS = pg.py pgdb.py
ifeq ($(with_python_compile), yes)
PY_COMPILED_SCRIPTS = $(PY_SCRIPTS:%.py=%.pyc) $(PY_SCRIPTS:%.py=%.pyo)
else
PY_COMPILED_SCRIPTS =
endif

all: all-lib $(PY_COMPILED_SCRIPTS)

all-lib: libpq-all

%.pyc: %.py
	$(PYTHON) -c "import py_compile; py_compile.compile(\"$<\")"

%.pyo: %.py
	$(PYTHON) -O -c "import py_compile; py_compile.compile(\"$<\")"

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(libpq_builddir) all

install-warning-msg := { \
echo "*** Skipping the installation of the Python interface module for lack"; \
echo "*** of permissions.  To install it, change to the directory"; \
echo "***     `pwd`,"; \
echo "*** become the appropriate user, and do '$(MAKE) install'."; }

install: all installdirs
	@if test -w $(DESTDIR)$(python_moduleexecdir) && test -w $(DESTDIR)$(python_moduledir); then \
	  echo "$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(python_moduleexecdir)/_pgmodule$(DLSUFFIX)"; \
	  $(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(python_moduleexecdir)/_pgmodule$(DLSUFFIX); \
	\
	  for i in $(PY_SCRIPTS) $(PY_COMPILED_SCRIPTS); do \
		echo $(INSTALL_DATA) $$i $(python_moduledir); \
		$(INSTALL_DATA) $$i $(python_moduledir); \
	  done \
	else \
	  $(install-warning-msg); \
	fi

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(python_moduleexecdir) $(DESTDIR)$(python_moduledir)

uninstall:
	rm -f $(DESTDIR)$(python_moduleexecdir)/_pgmodule$(DLSUFFIX) \
	      $(DESTDIR)$(python_moduledir)/pg.py \
	      $(DESTDIR)$(python_moduledir)/pgdb.py

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
