# $Header: /cvsroot/pgsql/src/backend/nls.mk,v 1.5 2003/07/19 20:32:12 tgl Exp $
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= cs de es hr hu ru sv tr zh_CN zh_TW
GETTEXT_FILES	:= + gettext-files
# elog should eventually be removed from this list:
GETTEXT_TRIGGERS:= elog:2 errmsg errdetail errhint errcontext postmaster_error yyerror

gettext-files:
	find $(srcdir)/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
