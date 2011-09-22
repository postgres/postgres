# src/backend/nls.mk
CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= de es fr ja pl pt_BR zh_CN zh_TW
GETTEXT_FILES	:= + gettext-files
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log \
    errdetail_plural:1,2 errhint errcontext \
    GUC_check_errmsg GUC_check_errdetail GUC_check_errhint \
    write_stderr yyerror parser_yyerror

gettext-files: distprep
	find $(srcdir)/ $(srcdir)/../port/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
