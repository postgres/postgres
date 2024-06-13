# src/bin/pg_rewind/nls.mk
CATALOG_NAME     = pg_rewind
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   datapagemap.c \
                   file_ops.c \
                   filemap.c \
                   libpq_source.c \
                   local_source.c \
                   parsexlog.c \
                   pg_rewind.c \
                   timeline.c \
                   xlogreader.c \
                   ../../common/controldata_utils.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../common/percentrepl.c \
                   ../../common/restricted_token.c \
                   ../../fe_utils/archive.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/recovery_gen.c \
                   ../../fe_utils/string_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   report_invalid_record:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
                   report_invalid_record:2:c-format
