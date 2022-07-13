# src/bin/pg_upgrade/nls.mk
CATALOG_NAME     = pg_upgrade
GETTEXT_FILES    = $(wildcard *.c)
GETTEXT_TRIGGERS = pg_fatal pg_log:2 prep_status prep_status_progress report_status:2
GETTEXT_FLAGS    = \
    pg_fatal:1:c-format \
    pg_log:2:c-format \
    prep_status:1:c-format \
    prep_status_progress:1:c-format \
    report_status:2:c-format
