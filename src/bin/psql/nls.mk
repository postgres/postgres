# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.14.2.1 2004/10/30 08:22:12 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr hu it nb pt_BR ru sl sv tr zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c sql_help.h
GETTEXT_TRIGGERS:= _ N_ psql_error simple_prompt
