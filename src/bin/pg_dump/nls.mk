# src/bin/pg_dump/nls.mk
CATALOG_NAME     = pg_dump
AVAIL_LANGUAGES  = cs de es fr it ja pl pt_BR ru zh_CN
GETTEXT_FILES    = pg_backup_archiver.c pg_backup_db.c pg_backup_custom.c \
                   pg_backup_null.c pg_backup_tar.c \
                   pg_backup_directory.c dumputils.c compress_io.c \
                   pg_dump.c common.c pg_dump_sort.c \
                   pg_restore.c pg_dumpall.c \
                   parallel.c parallel.h pg_backup_utils.c pg_backup_utils.h \
                   ../../common/exec.c ../../common/fe_memutils.c \
                   ../../common/wait_error.c
GETTEXT_TRIGGERS = write_msg:2 exit_horribly:2 simple_prompt \
                   ExecuteSqlCommand:3 ahlog:3 warn_or_exit_horribly:3
GETTEXT_FLAGS  = \
    write_msg:2:c-format \
    exit_horribly:2:c-format \
    ahlog:3:c-format \
    warn_or_exit_horribly:3:c-format
