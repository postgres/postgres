#
# Makefile for isolation tests
#

PGFILEDESC = "pg_isolation_regress/isolationtester - multi-client test driver"
PGAPPICON = win32

subdir = src/test/isolation
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) -I$(srcdir)/../regress $(CPPFLAGS)

OBJS = \
	$(WIN32RES) \
	isolationtester.o \
	specparse.o

all: isolationtester$(X) pg_isolation_regress$(X)

# Though we don't install these binaries, build them during installation
# (including temp-install).  Otherwise, "make -j check-world" and "make -j
# installcheck-world" would spawn multiple, concurrent builds in this
# directory.  Later builds would overwrite files while earlier builds are
# reading them, causing occasional failures.
install: | all

submake-regress:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress.o

pg_regress.o: | submake-regress
	rm -f $@ && $(LN_S) $(top_builddir)/src/test/regress/pg_regress.o .

pg_isolation_regress$(X): isolation_main.o pg_regress.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

isolationtester$(X): $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

distprep: specparse.c specscanner.c

# specscanner is compiled as part of specparse
specparse.o: specscanner.c

# specparse.c and specscanner.c are in the distribution tarball,
# so do not clean them here
clean distclean:
	rm -f isolationtester$(X) pg_isolation_regress$(X) $(OBJS) isolation_main.o
	rm -f pg_regress.o
	rm -rf $(pg_regress_clean_files)

maintainer-clean: distclean
	rm -f specparse.c specscanner.c

installcheck: all
	$(pg_isolation_regress_installcheck) --schedule=$(srcdir)/isolation_schedule

check: all
	$(pg_isolation_regress_check) --schedule=$(srcdir)/isolation_schedule

# Versions of the check tests that include the prepared-transactions test
# It only makes sense to run these if set up to use prepared transactions,
# via TEMP_CONFIG for the check case, or via the postgresql.conf for the
# installcheck case.
installcheck-prepared-txns: all temp-install
	$(pg_isolation_regress_installcheck) --schedule=$(srcdir)/isolation_schedule prepared-transactions

check-prepared-txns: all temp-install
	$(pg_isolation_regress_check) --schedule=$(srcdir)/isolation_schedule prepared-transactions
