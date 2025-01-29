# src/bin/pg_tde_alter_key_provider/Makefile

PGFILEDESC = "pg_tde_alter_key_provider"

subdir = src/bin/pg_tde_alter_key_provider
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	pg_tde_alter_key_provider.o 

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

OBJS += \
	$(top_srcdir)/src/fe_utils/simple_list.o \
	$(top_builddir)/src/libtde/libtde.a

override CPPFLAGS := -I$(top_srcdir)/contrib/pg_tde/src/include -I$(top_srcdir)/contrib/pg_tde/src/libkmip/libkmip/include $(CPPFLAGS)

all: pg_tde_alter_key_provider

pg_tde_alter_key_provider: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_tde_alter_key_provider$(X) '$(DESTDIR)$(bindir)/pg_tde_alter_key_provider$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_tde_alter_key_provider$(X)'

clean distclean:
	rm -f pg_tde_alter_key_provider$(X) $(OBJS) $(RMGRDESCSOURCES) xlogreader.c xlogstats.c
	rm -rf tmp_check