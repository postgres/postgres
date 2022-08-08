# src/bin/pg_verifybackup/nls.mk
CATALOG_NAME     = pg_verifybackup
AVAIL_LANGUAGES  = de el es fr ja ka ko ru sv uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   parse_manifest.c \
                   pg_verifybackup.c \
                   ../../common/fe_memutils.c \
                   ../../common/jsonapi.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   json_manifest_parse_failure:2 \
                   error_cb:2 \
                   report_backup_error:2 \
                   report_fatal_error
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   error_cb:2:c-format \
                   report_backup_error:2:c-format \
                   report_fatal_error:1:c-format
