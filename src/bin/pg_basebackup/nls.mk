# src/bin/pg_basebackup/nls.mk
CATALOG_NAME     = pg_basebackup
AVAIL_LANGUAGES  = de es fr it ja ka ru sv uk
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   bbstreamer_file.c \
                   bbstreamer_gzip.c \
                   bbstreamer_inject.c \
                   bbstreamer_lz4.c \
                   bbstreamer_tar.c \
                   bbstreamer_zstd.c \
                   pg_basebackup.c \
                   pg_receivewal.c \
                   pg_recvlogical.c \
                   receivelog.c \
                   streamutil.c \
                   walmethods.c \
                   ../../common/compression.c \
                   ../../common/fe_memutils.c \
                   ../../common/file_utils.c \
                   ../../fe_utils/option_utils.c \
                   ../../fe_utils/recovery_gen.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) simple_prompt tar_set_error
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
