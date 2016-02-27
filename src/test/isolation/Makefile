#
# Makefile for isolation tests
#

PGFILEDESC = "pg_isolation_regress/isolationtester - multi-client test driver"
PGAPPICON = win32

subdir = src/test/isolation
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) -I$(srcdir)/../regress $(CPPFLAGS)

OBJS =  specparse.o isolationtester.o $(WIN32RES)

all: isolationtester$(X) pg_isolation_regress$(X)

submake-regress:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress.o

pg_regress.o: | submake-regress
	rm -f $@ && $(LN_S) $(top_builddir)/src/test/regress/pg_regress.o .

pg_isolation_regress$(X): isolation_main.o pg_regress.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

isolationtester$(X): $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

distprep: specparse.c specscanner.c

# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  To be safe for parallel
# make, we must chain the dependencies like this.  The semicolon is
# important, otherwise make will choose the built-in rule for
# gram.y=>gram.c.

specparse.h: specparse.c ;

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
	./pg_isolation_regress --bindir='$(bindir)' $(EXTRA_REGRESS_OPTS) --inputdir=$(srcdir) --schedule=$(srcdir)/isolation_schedule

check: all
	$(with_temp_install) ./pg_isolation_regress --temp-instance=./tmp_check $(TEMP_CONF) --inputdir=$(srcdir) --bindir= $(EXTRA_REGRESS_OPTS) --schedule=$(srcdir)/isolation_schedule

# Versions of the check tests that include the prepared_transactions test
# It only makes sense to run these if set up to use prepared transactions,
# via TEMP_CONFIG for the check case, or via the postgresql.conf for the
# installcheck case.
installcheck-prepared-txns: all temp-install
	./pg_isolation_regress --bindir='$(bindir)' $(EXTRA_REGRESS_OPTS) --inputdir=$(srcdir) --schedule=$(srcdir)/isolation_schedule prepared-transactions

check-prepared-txns: all temp-install
	./pg_isolation_regress --temp-instance=./tmp_check $(TEMP_CONF) $(EXTRA_REGRESS_OPTS) --inputdir=$(srcdir) --schedule=$(srcdir)/isolation_schedule prepared-transactions
