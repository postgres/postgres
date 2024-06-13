# src/bin/pg_combinebackup/nls.mk
CATALOG_NAME     = pg_combinebackup
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   backup_label.c \
                   copy_file.c \
                   load_manifest.c \
                   pg_combinebackup.c \
                   reconstruct.c \
                   write_manifest.c \
                   ../../common/controldata_utils.c \
                   ../../common/cryptohash.c \
                   ../../common/cryptohash_openssl.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/jsonapi.c \
                   ../../common/parse_manifest.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   json_token_error:2 \
                   json_manifest_parse_failure:2 \
                   error_cb:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   json_token_error:2:c-format \
                   error_cb:2:c-format
