# $Header: /cvsroot/pgsql/src/bin/pg_config/Makefile,v 1.5 2002/09/09 18:35:04 petere Exp $

subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: pg_config

pg_config: pg_config.sh $(top_builddir)/src/Makefile.global Makefile
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@includedir@,$(includedir),g' \
	    -e 's,@includedir_server@,$(includedir_server),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e 's,@pkglibdir@,$(pkglibdir),g' \
	    -e "s|@configure@|$(configure_args)|g" \
	    -e 's,@version@,$(VERSION),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) pg_config $(DESTDIR)$(bindir)/pg_config

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_config

clean distclean maintainer-clean:
	rm -f pg_config
