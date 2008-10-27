# $Header: /cvsroot/pgsql/src/backend/nls.mk,v 1.9.2.3 2008/10/27 19:37:56 tgl Exp $
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= cs de es fr hr hu it nb pt_BR ru sv tr zh_CN zh_TW
GETTEXT_FILES	:= + gettext-files
GETTEXT_TRIGGERS:= errmsg errdetail errhint errcontext postmaster_error yyerror

gettext-files: distprep
	find $(srcdir)/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
