# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.4 2001/06/30 17:26:12 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= de fr sv
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c describe.c
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
