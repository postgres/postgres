#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# 'make install' installs only those headers needed for client-side
# programming.  'make install-all-headers' installs the whole contents
# of src/include.
#
# $Header: /cvsroot/pgsql/src/include/Makefile,v 1.7 2001/02/20 20:37:13 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# Headers needed by clients
srcdir_headers := c.h postgres_ext.h postgres_fe.h \
	libpq/pqcomm.h libpq/libpq-fs.h lib/dllist.h

builddir_headers := os.h config.h

HEADERS = $(srcdir_headers) $(builddir_headers)

# Subdirectories containing headers that install-all-headers should install
SUBDIRS = access bootstrap catalog commands executor lib libpq mb \
	nodes optimizer parser port regex rewrite storage tcop utils


all: $(HEADERS)


# Install only selected headers

install: all installdirs remove-old-headers
	for file in $(srcdir_headers); do \
	  $(INSTALL_DATA) $(srcdir)/$$file $(DESTDIR)$(includedir)/$$file || exit; \
	done
	for file in $(builddir_headers); do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(includedir)/$$file || exit; \
	done

# Automatically pick out the needed subdirectories for the include tree.
installdirs:
	$(mkinstalldirs) $(addprefix $(DESTDIR)$(includedir)/, $(sort $(dir $(HEADERS))))


# Install all headers

install-all-headers: all install-all-dirs
	for file in $(srcdir)/*.h; do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(includedir)/`basename $$file` || exit; \
	done
	for dir in $(SUBDIRS); do \
	  for file in $(srcdir)/$$dir/*.h; do \
	    $(INSTALL_DATA) $$file $(DESTDIR)$(includedir)/$$dir/`basename $$file` || exit; \
	  done \
	done
	for file in $(builddir_headers); do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(includedir)/$$file || exit; \
	done

install-all-dirs:
	$(mkinstalldirs) $(addprefix $(DESTDIR)$(includedir)/, $(SUBDIRS))


# Pre-7.1 Postgres installed some headers that are no longer installed by
# default.  If we see these headers in the target directory, zap them to
# avoid cross-version compile problems.  However, don't zap them if they
# match current sources (that means the user did install-all-headers).

remove-old-headers:
	for file in fmgr.h postgres.h access/attnum.h commands/trigger.h \
		executor/spi.h utils/elog.h utils/geo_decls.h utils/mcxt.h \
		utils/palloc.h; do \
		if cmp -s $(srcdir)/$$file $(DESTDIR)$(includedir)/$$file; \
		then \
			: ; \
		else \
			rm -f $(DESTDIR)$(includedir)/$$file; \
		fi \
	done


# This isn't a complete uninstall, but rm'ing everything under
# $(DESTDIR)$(includedir) is probably too drastic...
uninstall:
	rm -rf $(addprefix $(DESTDIR)$(includedir)/, $(HEADERS) $(SUBDIRS))

clean:
	rm -f utils/fmgroids.h parser/parse.h

distclean maintainer-clean: clean
	rm -f config.h dynloader.h os.h stamp-h
