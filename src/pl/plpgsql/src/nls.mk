# $PostgreSQL: pgsql/src/pl/plpgsql/src/nls.mk,v 1.1 2008/10/09 17:24:05 alvherre Exp $
CATALOG_NAME	:= plpgsql
AVAIL_LANGUAGES	:=
GETTEXT_FILES	:= pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scan.c
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext write_stderr yyerror

.PHONY: gettext-files
gettext-files: distprep
