# src/bin/pg_test_fsync/Makefile

PGFILEDESC = "pg_test_fsync - test various disk sync methods"
PGAPPICON = win32

subdir = src/bin/pg_test_fsync
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils

OBJS = pg_test_fsync.o $(WIN32RES)

all: pg_test_fsync

pg_test_fsync: $(OBJS) | submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_test_fsync$(X) '$(DESTDIR)$(bindir)/pg_test_fsync$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_test_fsync$(X)'

clean distclean maintainer-clean:
	rm -f pg_test_fsync$(X) $(OBJS)
