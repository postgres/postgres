# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.8.2.1 2003/09/07 04:37:04 momjian Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de es fr hu ru sv zh_CN zh_TW
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
