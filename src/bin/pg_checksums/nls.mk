# src/bin/pg_checksums/nls.mk
CATALOG_NAME     = pg_checksums
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_checksums.c \
                   ../../common/controldata_utils.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
