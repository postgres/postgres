# src/bin/pg_verifybackup/nls.mk
CATALOG_NAME     = pg_verifybackup
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_verifybackup.c \
                   ../../common/controldata_utils.c \
                   ../../common/cryptohash.c \
                   ../../common/cryptohash_openssl.c \
                   ../../common/fe_memutils.c \
                   ../../common/jsonapi.c \
                   ../../common/parse_manifest.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   json_token_error:2 \
                   json_manifest_parse_failure:2 \
                   error_cb:2 \
                   report_backup_error:2 \
                   report_fatal_error
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   json_token_error:2:c-format \
                   error_cb:2:c-format \
                   report_backup_error:2:c-format \
                   report_fatal_error:1:c-format
