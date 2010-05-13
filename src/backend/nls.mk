# $PostgreSQL: pgsql/src/backend/nls.mk,v 1.27.2.1 2010/05/13 10:49:44 petere Exp $
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= de es fr ja pt_BR tr zh_CN
GETTEXT_FILES	:= + gettext-files
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext write_stderr yyerror

gettext-files: distprep
	find $(srcdir)/ $(srcdir)/../port/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
