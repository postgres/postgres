# $PostgreSQL: pgsql/src/pl/plpgsql/src/nls.mk,v 1.11 2009/10/20 18:23:27 petere Exp $
CATALOG_NAME	:= plpgsql
AVAIL_LANGUAGES	:= de es fr it ja ro
GETTEXT_FILES	:= pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scan.c
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext yyerror plpgsql_yyerror

.PHONY: gettext-files
gettext-files: distprep
