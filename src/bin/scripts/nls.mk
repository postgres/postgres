# src/bin/scripts/nls.mk
CATALOG_NAME     = pgscripts
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) $(wildcard *.c) \
                   ../../fe_utils/parallel_slot.c \
                   ../../fe_utils/cancel.c ../../fe_utils/print.c \
                   ../../fe_utils/connect_utils.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/query_utils.c \
                   ../../common/fe_memutils.c ../../common/username.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) simple_prompt yesno_prompt
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
