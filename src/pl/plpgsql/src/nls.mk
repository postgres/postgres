# src/pl/plpgsql/src/nls.mk
CATALOG_NAME     = plpgsql
AVAIL_LANGUAGES  = cs de es fr it ja ko pl pt_BR ru sv tr uk vi zh_CN
GETTEXT_FILES    = pl_comp.c pl_exec.c pl_gram.c pl_funcs.c pl_handler.c pl_scanner.c
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS) yyerror plpgsql_yyerror
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS)
