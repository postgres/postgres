# $PostgreSQL: pgsql/src/pl/plpgsql/src/nls.mk,v 1.12.8.2 2010/09/16 19:09:38 petere Exp $
CATALOG_NAME	:= plpgsql
AVAIL_LANGUAGES	:= de es fr it ja ko pt_BR ro tr zh_CN zh_TW
GETTEXT_FILES	:= pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scanner.c
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext yyerror plpgsql_yyerror

.PHONY: gettext-files
gettext-files: distprep
