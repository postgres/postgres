# src/bin/psql/nls.mk
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr ja pt_BR sv tr zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h sql_help.c \
                   ../../port/exec.c
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
