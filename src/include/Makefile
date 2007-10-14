#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# 'make install' installs whole contents of src/include.
#
# $PostgreSQL: pgsql/src/include/Makefile,v 1.23 2007/10/14 17:07:51 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


all: pg_config.h pg_config_os.h


# Subdirectories containing headers for server-side dev
SUBDIRS = access bootstrap catalog commands executor lib libpq mb \
	nodes optimizer parser postmaster regex rewrite storage tcop \
	snowball snowball/libstemmer tsearch tsearch/dicts utils \
	port port/win32 port/win32_msvc port/win32_msvc/sys \
	port/win32/arpa port/win32/netinet port/win32/sys

# Install all headers
install: all installdirs
# These headers are needed by the public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/postgres_ext.h   '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq/libpq-fs.h '$(DESTDIR)$(includedir)/libpq'
	$(INSTALL_DATA) pg_config.h    '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) pg_config_os.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/pg_config_manual.h '$(DESTDIR)$(includedir)'
# These headers are needed by the not-so-public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/c.h            '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/port.h         '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/postgres_fe.h  '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/libpq/pqcomm.h '$(DESTDIR)$(includedir_internal)/libpq'
# These headers are needed for server-side development
	$(INSTALL_DATA) pg_config.h    '$(DESTDIR)$(includedir_server)'
	$(INSTALL_DATA) pg_config_os.h '$(DESTDIR)$(includedir_server)'
# We don't use INSTALL_DATA for performance reasons --- there are a lot of files
	cp $(srcdir)/*.h '$(DESTDIR)$(includedir_server)'/ || exit; \
	chmod $(INSTALL_DATA_MODE) '$(DESTDIR)$(includedir_server)'/*.h  || exit; \
	for dir in $(SUBDIRS); do \
	  cp $(srcdir)/$$dir/*.h '$(DESTDIR)$(includedir_server)'/$$dir/ || exit; \
	  chmod $(INSTALL_DATA_MODE) '$(DESTDIR)$(includedir_server)'/$$dir/*.h  || exit; \
	done

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(includedir)/libpq' '$(DESTDIR)$(includedir_internal)/libpq'
	$(mkinstalldirs) $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SUBDIRS))


uninstall:
	rm -f $(addprefix '$(DESTDIR)$(includedir)'/, pg_config.h pg_config_os.h pg_config_manual.h postgres_ext.h libpq/libpq-fs.h)
	rm -f $(addprefix '$(DESTDIR)$(includedir_internal)'/, c.h port.h postgres_fe.h libpq/pqcomm.h)
# heuristic...
	rm -rf $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SUBDIRS) *.h)


clean:
	rm -f utils/fmgroids.h parser/parse.h

distclean maintainer-clean: clean
	rm -f pg_config.h dynloader.h pg_config_os.h stamp-h
