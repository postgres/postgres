# src/bin/pg_archivecleanup/Makefile

PGFILEDESC = "pg_archivecleanup - cleans archive when used with streaming replication"
PGAPPICON = win32

subdir = src/bin/pg_archivecleanup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS	= pg_archivecleanup.o $(WIN32RES)

all: pg_archivecleanup

pg_archivecleanup: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_archivecleanup$(X) '$(DESTDIR)$(bindir)/pg_archivecleanup$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_archivecleanup$(X)'

clean distclean maintainer-clean:
	rm -f pg_archivecleanup$(X) $(OBJS)
