# $PostgreSQL: pgsql/src/bin/psql/nls.mk,v 1.23.2.1 2009/09/03 21:01:10 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr it ja pt_BR sv tr
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h \
                   ../../port/exec.c
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
