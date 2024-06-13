# src/bin/pg_waldump/nls.mk
CATALOG_NAME     = pg_waldump
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_waldump.c \
                   xlogreader.c \
                   xlogstats.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   report_invalid_record:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   report_invalid_record:2:c-format
