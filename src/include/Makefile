#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# 'make install' installs whole contents of src/include.
#
# $PostgreSQL: pgsql/src/include/Makefile,v 1.15 2004/10/16 22:52:39 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


all: pg_config.h pg_config_os.h


# Subdirectories containing headers for server-side dev
SUBDIRS = access bootstrap catalog commands executor lib libpq mb \
	nodes optimizer parser port regex rewrite storage tcop utils

# Install all headers
install: all installdirs remove-old-headers
# These headers are needed by the public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/postgres_ext.h   $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/libpq/libpq-fs.h $(DESTDIR)$(includedir)/libpq
	$(INSTALL_DATA) pg_config.h    $(DESTDIR)$(includedir)
	$(INSTALL_DATA) pg_config_os.h $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/pg_config_manual.h $(DESTDIR)$(includedir)
# These headers are needed by the not-so-public headers of the interfaces.
	$(INSTALL_DATA) $(srcdir)/c.h            $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/port.h         $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/postgres_fe.h  $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/libpq/pqcomm.h $(DESTDIR)$(includedir_internal)/libpq
# These headers are needed for server-side development
	$(INSTALL_DATA) pg_config.h    $(DESTDIR)$(includedir_server)
	$(INSTALL_DATA) pg_config_os.h $(DESTDIR)$(includedir_server)
	for file in $(srcdir)/*.h; do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(includedir_server)/`basename $$file` || exit; \
	done
	for dir in $(SUBDIRS); do \
	  for file in $(srcdir)/$$dir/*.h; do \
	    $(INSTALL_DATA) $$file $(DESTDIR)$(includedir_server)/$$dir/`basename $$file` || exit; \
	  done \
	done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(includedir)/libpq $(DESTDIR)$(includedir_internal)/libpq $(DESTDIR)$(includedir_internal)/lib
	$(mkinstalldirs) $(addprefix $(DESTDIR)$(includedir_server)/, $(SUBDIRS))


# Pre-7.1 Postgres installed some headers that are no longer installed by
# default.  If we see these headers in the target directory, zap them to
# avoid cross-version compile problems.  However, don't zap them if they
# match current sources (that means the user did install-all-headers).

remove-old-headers:
	for file in fmgr.h postgres.h access/attnum.h commands/trigger.h \
		executor/spi.h utils/elog.h utils/geo_decls.h utils/mcxt.h \
		utils/palloc.h; do \
		if cmp -s $(srcdir)/$$file $(DESTDIR)$(includedir)/$$file 2>/dev/null; \
		then \
			: ; \
		else \
			rm -f $(DESTDIR)$(includedir)/$$file; \
		fi ; \
	done


uninstall:
	rm -f $(addprefix $(DESTDIR)$(includedir)/, pg_config.h pg_config_os.h postgres_ext.h libpq/libpq-fs.h)
	rm -f $(addprefix $(DESTDIR)$(includedir_internal)/, c.h postgres_fe.h libpq/pqcomm.h)
# heuristic...
	rm -rf $(addprefix $(DESTDIR)$(includedir_server)/, $(SUBDIRS) *.h)


clean:
	rm -f utils/fmgroids.h parser/parse.h

distclean maintainer-clean: clean
	rm -f pg_config.h dynloader.h pg_config_os.h stamp-h
