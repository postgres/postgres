# src/bin/pg_rewind/nls.mk
CATALOG_NAME     = pg_rewind
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) $(wildcard *.c) ../../common/fe_memutils.c ../../common/restricted_token.c ../../fe_utils/archive.c ../../fe_utils/recovery_gen.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) report_invalid_record:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
    report_invalid_record:2:c-format
