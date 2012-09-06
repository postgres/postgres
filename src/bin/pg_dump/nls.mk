# src/bin/pg_dump/nls.mk
CATALOG_NAME     = pg_dump
AVAIL_LANGUAGES  = de es fr ja ru
GETTEXT_FILES    = pg_backup_archiver.c pg_backup_db.c pg_backup_custom.c \
                   pg_backup_null.c pg_backup_tar.c \
                   pg_backup_directory.c dumpmem.c dumputils.c compress_io.c \
                   pg_dump.c common.c pg_dump_sort.c \
                   pg_restore.c pg_dumpall.c \
                   ../../port/exec.c
GETTEXT_TRIGGERS = write_msg:2 exit_horribly:2 simple_prompt \
                   ExecuteSqlCommand:3 ahlog:3 warn_or_exit_horribly:3
GETTEXT_FLAGS  = \
    write_msg:2:c-format \
    exit_horribly:2:c-format \
    ahlog:3:c-format \
    warn_or_exit_horribly:3:c-format
