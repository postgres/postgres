# $Header: /cvsroot/pgsql/src/bin/psql/nls.mk,v 1.1 2001/06/02 18:25:18 petere Exp $
CATALOG_NAME	:= psql
AVAIL_LANGUAGES	:= de
GETTEXT_FILES	:= command.c common.c copy.c help.c input.c large_obj.c \
                   mainloop.c print.c startup.c
                   # describe.c needs work
GETTEXT_TRIGGERS:= _ psql_error simple_prompt
