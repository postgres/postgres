# src/bin/scripts/nls.mk
CATALOG_NAME     = pgscripts
AVAIL_LANGUAGES  = cs de es fr ja ko ru sv tr uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   createdb.c createuser.c \
                   dropdb.c dropuser.c \
                   clusterdb.c vacuumdb.c reindexdb.c \
                   pg_isready.c \
                   common.c \
                   scripts_parallel.c \
                   ../../fe_utils/cancel.c ../../fe_utils/print.c \
                   ../../common/fe_memutils.c ../../common/username.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) simple_prompt yesno_prompt
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
