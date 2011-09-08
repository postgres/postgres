# src/bin/scripts/nls.mk
CATALOG_NAME    := pgscripts
AVAIL_LANGUAGES := cs de es fr it ja ko pl pt_BR ro sv ta tr zh_CN zh_TW
GETTEXT_FILES   := createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   common.c
GETTEXT_TRIGGERS:= _ simple_prompt yesno_prompt
