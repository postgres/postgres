# $PostgreSQL: pgsql/src/bin/psql/nls.mk,v 1.17 2004/09/14 05:50:33 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fa fr hu it nb pt_BR ru sk sl sv zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
