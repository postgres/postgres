# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.1 2003/03/18 22:19:47 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := 
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
