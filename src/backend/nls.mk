# $Header: /cvsroot/pgsql/src/backend/nls.mk,v 1.6 2003/07/28 00:25:21 tgl Exp $
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= cs de es hr hu ru sv tr zh_CN zh_TW
GETTEXT_FILES	:= + gettext-files
# you can add "elog:2" and "errmsg_internal" to this list if you want to
# include internal messages in the translation list.
GETTEXT_TRIGGERS:= errmsg errdetail errhint errcontext postmaster_error yyerror

gettext-files:
	find $(srcdir)/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
