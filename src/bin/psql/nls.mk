# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.2 2001/06/11 18:23:33 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= de fr
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c
                   # describe.c needs work
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
