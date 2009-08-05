# Postgres documentation makefile
# $PostgreSQL: pgsql/doc/src/Makefile,v 1.36 2009/08/05 19:31:50 alvherre Exp $

subdir = doc/src
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

clean distclean maintainer-clean:
	rm -f *.tar *.gz
	$(MAKE) -C sgml $@

postgres.tar:
	$(MAKE) -C sgml html JADEFLAGS='-V html-manifest'
	cd sgml && $(TAR) -cf ../$@ `cat HTML.manifest` `echo *.gif | grep -v '\*'` *.css

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -cf $@ -C sgml man1 man3 man7
