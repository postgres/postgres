# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.6 2001/09/06 11:10:46 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= cs de fr ru sv
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
