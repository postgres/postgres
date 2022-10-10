# src/bin/pg_archivecleanup/nls.mk
CATALOG_NAME     = pg_archivecleanup
AVAIL_LANGUAGES  = cs de el es fr ja ka ko pt_BR ru sv tr uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) pg_archivecleanup.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
