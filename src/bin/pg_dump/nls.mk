# src/bin/pg_dump/nls.mk
CATALOG_NAME     = pg_dump
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) $(wildcard *.c) \
                   ../../common/exec.c ../../common/fe_memutils.c \
                   ../../common/wait_error.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   simple_prompt \
                   ExecuteSqlCommand:3 warn_or_exit_horribly:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
    warn_or_exit_horribly:2:c-format
