# src/bin/psql/nls.mk
CATALOG_NAME     = psql
AVAIL_LANGUAGES  = cs de es fr he it ja ko pl pt_BR ru sv zh_CN zh_TW
GETTEXT_FILES    = command.c common.c copy.c crosstabview.c help.c input.c large_obj.c \
                   mainloop.c psqlscanslash.c startup.c \
                   describe.c sql_help.h sql_help.c \
                   tab-complete.c variables.c \
                   ../../fe_utils/print.c ../../fe_utils/psqlscan.c \
                   ../../common/exec.c ../../common/fe_memutils.c ../../common/username.c \
                   ../../common/wait_error.c
GETTEXT_TRIGGERS = N_ psql_error simple_prompt write_error
GETTEXT_FLAGS    = psql_error:1:c-format write_error:1:c-format
