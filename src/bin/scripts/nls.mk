# $Header: /cvsroot/pgsql/src/bin/scripts/nls.mk,v 1.11.2.2 2004/10/30 08:22:14 petere Exp $
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es fr it pt_BR ru sl sv tr zh_CN
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt
