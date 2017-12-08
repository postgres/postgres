# src/bin/pg_upgrade/nls.mk
CATALOG_NAME     = pg_upgrade
AVAIL_LANGUAGES  =fr ru
GETTEXT_FILES    = check.c controldata.c dump.c exec.c file.c function.c \
                   info.c option.c parallel.c pg_upgrade.c relfilenode.c \
                   server.c tablespace.c util.c version.c
GETTEXT_TRIGGERS = pg_fatal pg_log:2 prep_status report_status:2
GETTEXT_FLAGS    = \
    pg_fatal:1:c-format \
    pg_log:2:c-format \
    prep_status:1:c-format \
    report_status:2:c-format
