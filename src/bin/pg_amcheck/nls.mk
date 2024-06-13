# src/bin/pg_amcheck/nls.mk
CATALOG_NAME     = pg_amcheck
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_amcheck.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/username.c \
                   ../../fe_utils/cancel.c \
                   ../../fe_utils/connect_utils.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/parallel_slot.c \
                   ../../fe_utils/query_utils.c \
                   ../../fe_utils/string_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   log_no_match
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   log_no_match:1:c-format
