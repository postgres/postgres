# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.7 2003/09/15 20:41:59 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de ru sv zh_CN
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
