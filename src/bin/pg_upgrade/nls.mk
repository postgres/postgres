# src/bin/pg_upgrade/nls.mk
CATALOG_NAME     = pg_upgrade
GETTEXT_FILES    = check.c \
                   controldata.c \
                   dump.c \
                   exec.c \
                   file.c \
                   function.c \
                   info.c \
                   option.c \
                   parallel.c \
                   pg_upgrade.c \
                   relfilenumber.c \
                   server.c \
                   tablespace.c \
                   util.c \
                   version.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/restricted_token.c \
                   ../../common/username.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/string_utils.c
GETTEXT_TRIGGERS = pg_fatal \
                   pg_log:2 \
                   prep_status \
                   prep_status_progress \
                   report_status:2
GETTEXT_FLAGS    = pg_fatal:1:c-format \
                   pg_log:2:c-format \
                   prep_status:1:c-format \
                   prep_status_progress:1:c-format \
                   report_status:2:c-format
