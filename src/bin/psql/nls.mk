# src/bin/psql/nls.mk
CATALOG_NAME     = psql
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) $(wildcard *.c) \
                   ../../fe_utils/cancel.c ../../fe_utils/print.c ../../fe_utils/psqlscan.c \
                   ../../common/exec.c ../../common/fe_memutils.c ../../common/username.c \
                   ../../common/wait_error.c ../../port/thread.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   HELP0 HELPN N_ simple_prompt simple_prompt_extended
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   HELPN:1:c-format
