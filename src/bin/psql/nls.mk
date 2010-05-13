# $PostgreSQL: pgsql/src/bin/psql/nls.mk,v 1.23.2.2 2010/05/13 10:50:05 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr it ja pt_BR sv tr zh_CN
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h \
                   ../../port/exec.c
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
