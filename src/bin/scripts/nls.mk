# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.11 2003/10/08 20:35:39 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es it pt_BR ru sl sv zh_CN
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
