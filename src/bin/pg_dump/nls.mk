# src/bin/pg_dump/nls.mk
CATALOG_NAME     = pg_dump
AVAIL_LANGUAGES  = cs de el es fr it ja ka ko ru sv tr uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_backup_archiver.c pg_backup_db.c pg_backup_custom.c \
                   pg_backup_null.c pg_backup_tar.c \
                   pg_backup_directory.c dumputils.c compress_io.c \
                   pg_dump.c common.c pg_dump_sort.c \
                   pg_restore.c pg_dumpall.c \
                   parallel.c parallel.h pg_backup_utils.c pg_backup_utils.h \
                   ../../common/exec.c ../../common/fe_memutils.c \
                   ../../common/wait_error.c \
                   ../../fe_utils/option_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) \
                   simple_prompt \
                   ExecuteSqlCommand:3 warn_or_exit_horribly:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
    warn_or_exit_horribly:2:c-format
