# src/bin/pg_walsummary/nls.mk
CATALOG_NAME     = pg_walsummary
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_walsummary.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
