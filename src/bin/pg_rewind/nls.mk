# src/bin/pg_rewind/nls.mk
CATALOG_NAME     = pg_rewind
AVAIL_LANGUAGES  =de
GETTEXT_FILES    = copy_fetch.c datapagemap.c fetch.c file_ops.c filemap.c libpq_fetch.c logging.c parsexlog.c pg_rewind.c timeline.c ../../common/fe_memutils.c ../../common/restricted_token.c xlogreader.c

GETTEXT_TRIGGERS = pg_log:2 pg_fatal report_invalid_record:2
GETTEXT_FLAGS    = pg_log:2:c-format \
    pg_fatal:1:c-format \
    report_invalid_record:2:c-format
