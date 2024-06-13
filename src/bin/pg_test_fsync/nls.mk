# src/bin/pg_test_fsync/nls.mk
CATALOG_NAME     = pg_test_fsync
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) pg_test_fsync.c ../../common/fe_memutils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) die
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
