# src/pl/plpython/Makefile

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# On Windows we have to remove -lpython from the link since we are
# building our own
ifeq ($(PORTNAME), win32)
override python_libspec =
endif

override CPPFLAGS := -I. -I$(srcdir) $(python_includespec) $(CPPFLAGS)

rpathdir = $(python_libdir)

PGFILEDESC = "PL/Python - procedural language"

NAME = plpython$(python_majorversion)

OBJS = \
	$(WIN32RES) \
	plpy_cursorobject.o \
	plpy_elog.o \
	plpy_exec.o \
	plpy_main.o \
	plpy_planobject.o \
	plpy_plpymodule.o \
	plpy_procedure.o \
	plpy_resultobject.o \
	plpy_spi.o \
	plpy_subxactobject.o \
	plpy_typeio.o \
	plpy_util.o

DATA = $(NAME)u.control $(NAME)u--1.0.sql

# header files to install - it's not clear which of these might be needed
# so install them all.
INCS = 	plpython.h \
	plpy_cursorobject.h \
	plpy_elog.h \
	plpy_exec.h \
	plpy_main.h \
	plpy_planobject.h \
	plpy_plpymodule.h \
	plpy_procedure.h \
	plpy_resultobject.h \
	plpy_spi.h \
	plpy_subxactobject.h \
	plpy_typeio.h \
	plpy_util.h

# Python on win32 ships with import libraries only for Microsoft Visual C++,
# which are not compatible with mingw gcc. Therefore we need to build a
# new import library to link with.
ifeq ($(PORTNAME), win32)

pytverstr=$(subst .,,${python_version})
PYTHONDLL=$(subst \,/,$(WINDIR))/system32/python${pytverstr}.dll

OBJS += libpython${pytverstr}.a

libpython${pytverstr}.a: python${pytverstr}.def
	dlltool --dllname python${pytverstr}.dll --def python${pytverstr}.def --output-lib libpython${pytverstr}.a

python${pytverstr}.def:
	gendef - $(PYTHONDLL) > $@

endif # win32


SHLIB_LINK = $(python_libspec) $(python_additional_libs) $(filter -lintl,$(LIBS))

REGRESS_OPTS = --dbname=$(PL_TESTDB)

REGRESS = \
	plpython_schema \
	plpython_populate \
	plpython_test \
	plpython_do \
	plpython_global \
	plpython_import \
	plpython_spi \
	plpython_newline \
	plpython_void \
	plpython_call \
	plpython_params \
	plpython_setof \
	plpython_record \
	plpython_trigger \
	plpython_types \
	plpython_error \
	plpython_ereport \
	plpython_unicode \
	plpython_quote \
	plpython_composite \
	plpython_subtransaction \
	plpython_transaction \
	plpython_drop

include $(top_srcdir)/src/Makefile.shlib

all: all-lib

# Ensure parallel safety if a build is started in this directory
$(OBJS): | submake-generated-headers

install: all install-lib install-data

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension' '$(DESTDIR)$(includedir_server)'

uninstall: uninstall-lib uninstall-data

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(INCS)) '$(DESTDIR)$(includedir_server)'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))
	rm -f $(addprefix '$(DESTDIR)$(includedir_server)'/, plpython.h plpy_util.h)

.PHONY: install-data uninstall-data


check: submake-pg-regress
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)

installcheck: submake-pg-regress
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)


.PHONY: submake-pg-regress
submake-pg-regress: | submake-generated-headers
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean: clean-lib
	rm -f $(OBJS)
	rm -rf $(pg_regress_clean_files)
ifeq ($(PORTNAME), win32)
	rm -f python${pytverstr}.def
endif


# Force this dependency to be known even without dependency info built:
plpy_plpymodule.o: spiexceptions.h

spiexceptions.h: $(top_srcdir)/src/backend/utils/errcodes.txt generate-spiexceptions.pl
	$(PERL) $(srcdir)/generate-spiexceptions.pl $< > $@

distprep: spiexceptions.h

maintainer-clean: distclean
	rm -f spiexceptions.h
