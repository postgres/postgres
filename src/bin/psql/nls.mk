# $PostgreSQL: pgsql/src/bin/psql/nls.mk,v 1.15 2003/11/29 19:52:07 pgsql Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr hu it nb pt_BR ru sl sv zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
