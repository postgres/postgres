# src/bin/pg_archivecleanup/nls.mk
CATALOG_NAME     = pg_archivecleanup
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) pg_archivecleanup.c ../../common/fe_memutils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
