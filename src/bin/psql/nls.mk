# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.7.4.1 2003/01/04 10:26:23 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de fr hu ru sv zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
