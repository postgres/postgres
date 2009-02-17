# $PostgreSQL: pgsql/src/pl/plpgsql/src/nls.mk,v 1.5 2009/02/17 12:59:35 petere Exp $
CATALOG_NAME	:= plpgsql
AVAIL_LANGUAGES	:= es
GETTEXT_FILES	:= pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scan.c
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext validate_tupdesc_compat:3 yyerror plpgsql_yyerror

.PHONY: gettext-files
gettext-files: distprep
