# src/bin/pg_basebackup/nls.mk
CATALOG_NAME     = pg_basebackup
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) $(wildcard *.c) \
                   ../../common/compression.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/recovery_gen.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) simple_prompt tar_set_error
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
