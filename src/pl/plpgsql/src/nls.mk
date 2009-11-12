# $PostgreSQL: pgsql/src/pl/plpgsql/src/nls.mk,v 1.12 2009/11/12 00:13:00 tgl Exp $
CATALOG_NAME	:= plpgsql
AVAIL_LANGUAGES	:= de es fr it ja ro
GETTEXT_FILES	:= pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scanner.c
GETTEXT_TRIGGERS:= _ errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext yyerror plpgsql_yyerror

.PHONY: gettext-files
gettext-files: distprep
