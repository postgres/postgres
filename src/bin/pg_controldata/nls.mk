# src/bin/pg_controldata/nls.mk
CATALOG_NAME     = pg_controldata
GETTEXT_FILES    = pg_controldata.c \
                   ../../common/controldata_utils.c \
                   ../../common/fe_memutils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
