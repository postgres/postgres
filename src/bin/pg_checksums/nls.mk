# src/bin/pg_checksums/nls.mk
CATALOG_NAME     = pg_checksums
AVAIL_LANGUAGES  = de el es fr it ja ka ko pt_BR ru sv uk
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_checksums.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
