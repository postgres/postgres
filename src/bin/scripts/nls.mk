# src/bin/scripts/nls.mk
CATALOG_NAME     = pgscripts
AVAIL_LANGUAGES  = cs de es fr it ja pl pt_BR ru zh_CN
GETTEXT_FILES    = createdb.c createlang.c createuser.c \
                   dropdb.c droplang.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   pg_isready.c \
                   common.c \
                   ../../common/fe_memutils.c
GETTEXT_TRIGGERS = simple_prompt yesno_prompt
