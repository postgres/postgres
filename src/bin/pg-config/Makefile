# $Header: /cvsroot/pgsql/src/bin/pg-config/Attic/Makefile,v 1.3 2000/09/17 13:02:34 petere Exp $

subdir = src/bin/pg-config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: pg-config

pg-config: pg-config.sh $(top_builddir)/config.status $(top_builddir)/src/Makefile.global Makefile
	configure=`sed -n '7s,^# [^ ]*configure *,,p' $(top_builddir)/config.status` && \
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@includedir@,$(includedir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e "s,@configure@,$$configure,g" \
	    -e 's,@version@,$(VERSION),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) pg-config $(DESTDIR)$(bindir)/pg-config

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg-config

clean distclean maintainer-clean:
	rm -f pg-config
