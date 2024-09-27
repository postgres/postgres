# src/bin/pg_verifybackup/Makefile

PGFILEDESC = "pg_verifybackup - verify a backup against using a backup manifest"
PGAPPICON = win32

# make these available to TAP test scripts
export TAR
# Note that GZIP cannot be used directly as this environment variable is
# used by the command "gzip" to pass down options, so stick with a different
# name.
export GZIP_PROGRAM=$(GZIP)
export LZ4=$(LZ4)
export ZSTD=$(ZSTD)

subdir = src/bin/pg_verifybackup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# We need libpq only because fe_utils does.
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	astreamer_verify.o \
	pg_verifybackup.o

all: pg_verifybackup

pg_verifybackup: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_verifybackup$(X) '$(DESTDIR)$(bindir)/pg_verifybackup$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_verifybackup$(X)'

clean distclean:
	rm -f pg_verifybackup$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
