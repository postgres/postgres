#
# Makefile for isolation tests
#

subdir = src/test/isolation
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# where to find psql for testing an existing installation
PSQLDIR = $(bindir)

ifeq ($(PORTNAME), win32)
LDLIBS += -lws2_32
endif

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) -I$(srcdir)/../regress $(CPPFLAGS)
override LDLIBS := $(libpq_pgport) $(LDLIBS)

OBJS =  specparse.o isolationtester.o

all: isolationtester$(X) pg_isolation_regress$(X)

pg_regress.o:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress.o
	rm -f $@ && $(LN_S) $(top_builddir)/src/test/regress/pg_regress.o .

pqsignal.o:
	$(MAKE) -C $(top_builddir)/src/test/regress pqsignal.o
	rm -f $@ && $(LN_S) $(top_builddir)/src/test/regress/pqsignal.o .

pg_isolation_regress$(X): isolation_main.o pg_regress.o pqsignal.o
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

isolationtester$(X): $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

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

specparse.c: specparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

specscanner.c: specscanner.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

# specparse.c and specscanner.c are in the distribution tarball,
# so do not clean them here
clean distclean:
	rm -f isolationtester$(X) pg_isolation_regress$(X) $(OBJS) isolation_main.o
	rm -f pg_regress.o pqsignal.o
	rm -rf $(pg_regress_clean_files)

maintainer-clean: distclean
	rm -f specparse.c specscanner.c

installcheck: all
	./pg_isolation_regress --psqldir='$(PSQLDIR)' --inputdir=$(srcdir) --schedule=$(srcdir)/isolation_schedule

# Version of the install check test that includes the prepared_transactions test
# It only makes sense to run this if set up to use prepared transactions,
# via the postgresql.conf.
installcheck-prepared-txns: all
	./pg_isolation_regress --psqldir='$(PSQLDIR)' --inputdir=$(srcdir) --schedule=$(srcdir)/isolation_schedule prepared-transactions


# We can't support "make check" because isolationtester requires libpq, and
# in fact (on typical platforms using shared libraries) requires libpq to
# already be installed.  You could run "make install" and then run a check
# using a temp installation, but there seems little point in that.
check:
	@echo "'make check' is not supported."
	@echo "Install PostgreSQL, then 'make installcheck' instead."
