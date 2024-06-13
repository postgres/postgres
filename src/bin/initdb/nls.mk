# src/bin/initdb/nls.mk
CATALOG_NAME     = initdb
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   findtimezone.c \
                   initdb.c \
                   ../../common/exec.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/pgfnames.c \
                   ../../common/restricted_token.c \
                   ../../common/rmtree.c \
                   ../../common/username.c \
                   ../../common/wait_error.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/string_utils.c \
                   ../../port/dirmod.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) simple_prompt
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
