# $Header: /cvsroot/pgsql/src/bin/pg_config/Makefile,v 1.1 2000/10/10 22:01:55 momjian Exp $

subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: pg_config

pg_config: pg_config.sh $(top_builddir)/config.status $(top_builddir)/src/Makefile.global Makefile
	configure=`sed -n '7s,^# [^ ]*configure *,,p' $(top_builddir)/config.status` && \
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@includedir@,$(includedir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e "s,@configure@,$$configure,g" \
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
