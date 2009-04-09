# $PostgreSQL: pgsql/src/bin/psql/nls.mk,v 1.22 2009/04/09 19:38:52 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fa fr hu it ja ko nb pt_BR ro ru sk sl sv tr zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h \
                   ../../port/exec.c
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
