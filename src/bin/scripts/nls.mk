# src/bin/scripts/nls.mk
CATALOG_NAME     = pgscripts
AVAIL_LANGUAGES  = cs de es fr he it ja ko pl pt_BR ru sv zh_CN
GETTEXT_FILES    = createdb.c createuser.c \
                   dropdb.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   pg_isready.c \
                   common.c \
                   ../../fe_utils/print.c \
                   ../../common/fe_memutils.c ../../common/username.c
GETTEXT_TRIGGERS = simple_prompt yesno_prompt
