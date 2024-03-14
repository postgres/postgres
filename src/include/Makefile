#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# 'make install' installs whole contents of src/include.
#
# src/include/Makefile
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


all: pg_config.h pg_config_ext.h pg_config_os.h


# Subdirectories containing installable headers
SUBDIRS = access archive bootstrap catalog commands common datatype \
	executor fe_utils foreign jit \
	lib libpq mb nodes optimizer parser partitioning postmaster \
	regex replication rewrite \
	statistics storage tcop snowball snowball/libstemmer tsearch \
	tsearch/dicts utils port port/atomics port/win32 port/win32_msvc \
	port/win32_msvc/sys port/win32/arpa port/win32/netinet \
	port/win32/sys portability

# Install all headers
install: all installdirs
# These headers are needed by the public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/postgres_ext.h   '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq/libpq-fs.h '$(DESTDIR)$(includedir)/libpq'
	$(INSTALL_DATA) pg_config.h     '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) pg_config_ext.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) pg_config_os.h  '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/pg_config_manual.h '$(DESTDIR)$(includedir)'
# These headers are needed by the not-so-public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/c.h            '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/port.h         '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/postgres_fe.h  '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/libpq/pqcomm.h '$(DESTDIR)$(includedir_internal)/libpq'
	$(INSTALL_DATA) $(srcdir)/libpq/protocol.h '$(DESTDIR)$(includedir_internal)/libpq'
# These headers are needed for server-side development
	$(INSTALL_DATA) pg_config.h     '$(DESTDIR)$(includedir_server)'
	$(INSTALL_DATA) pg_config_ext.h '$(DESTDIR)$(includedir_server)'
	$(INSTALL_DATA) pg_config_os.h  '$(DESTDIR)$(includedir_server)'
	$(INSTALL_DATA) nodes/nodetags.h '$(DESTDIR)$(includedir_server)/nodes'
	$(INSTALL_DATA) utils/errcodes.h '$(DESTDIR)$(includedir_server)/utils'
	$(INSTALL_DATA) utils/fmgroids.h '$(DESTDIR)$(includedir_server)/utils'
	$(INSTALL_DATA) utils/fmgrprotos.h '$(DESTDIR)$(includedir_server)/utils'
	$(INSTALL_DATA) $(srcdir)/*.h '$(DESTDIR)$(includedir_server)'
	for dir in $(SUBDIRS); do \
	  $(INSTALL_DATA) $(srcdir)/$$dir/*.h '$(DESTDIR)$(includedir_server)'/$$dir || exit; \
	done
ifeq ($(vpath_build),yes)
	for file in storage/lwlocknames.h utils/probes.h utils/wait_event_types.h; do \
	  $(INSTALL_DATA) $$file '$(DESTDIR)$(includedir_server)'/$$file || exit; \
	done
endif
	$(MAKE) -C catalog install

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(includedir)/libpq' '$(DESTDIR)$(includedir_internal)/libpq'
	$(MKDIR_P) $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SUBDIRS))


uninstall:
	rm -f $(addprefix '$(DESTDIR)$(includedir)'/, pg_config.h pg_config_ext.h pg_config_os.h pg_config_manual.h postgres_ext.h libpq/libpq-fs.h)
	rm -f $(addprefix '$(DESTDIR)$(includedir_internal)'/, c.h port.h postgres_fe.h libpq/pqcomm.h libpq/protocol.h)
# heuristic...
	rm -rf $(addprefix '$(DESTDIR)$(includedir_server)'/, $(SUBDIRS) *.h)
	$(MAKE) -C catalog uninstall


clean:
	rm -f utils/fmgroids.h utils/fmgrprotos.h utils/errcodes.h utils/header-stamp
	rm -f storage/lwlocknames.h utils/probes.h utils/wait_event_types.h
	rm -f nodes/nodetags.h nodes/header-stamp
	$(MAKE) -C catalog clean

distclean: clean
	rm -f pg_config.h pg_config_ext.h pg_config_os.h stamp-h stamp-ext-h
