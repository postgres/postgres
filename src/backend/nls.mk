CATALOG_NAME	:= postgres
AVAIL_LANGUAGES	:= cs de es hu ru sv zh_CN zh_TW
GETTEXT_FILES	:= + gettext-files
GETTEXT_TRIGGERS:= elog:2 postmaster_error yyerror

gettext-files:
	find $(srcdir)/ -name '*.c' -print >$@

my-maintainer-clean:
	rm -f gettext-files

.PHONY: my-maintainer-clean
maintainer-clean: my-maintainer-clean
