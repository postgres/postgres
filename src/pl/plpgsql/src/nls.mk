# src/pl/plpgsql/src/nls.mk
CATALOG_NAME     = plpgsql
GETTEXT_FILES    = $(wildcard *.c)
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS) yyerror plpgsql_yyerror
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS)
