# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.2 2003/06/18 12:19:11 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := 
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
