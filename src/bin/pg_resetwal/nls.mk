# src/bin/pg_resetwal/nls.mk
CATALOG_NAME     = pg_resetwal
AVAIL_LANGUAGES  = cs de el es fr it ja ka ko pt_BR ru sv uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) pg_resetwal.c ../../common/restricted_token.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
