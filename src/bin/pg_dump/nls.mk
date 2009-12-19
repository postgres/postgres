# $PostgreSQL: pgsql/src/bin/pg_dump/nls.mk,v 1.22 2009/12/19 20:23:25 petere Exp $
CATALOG_NAME	:= pg_dump
AVAIL_LANGUAGES	:= de es fr it ja pt_BR sv tr
GETTEXT_FILES	:= pg_dump.c common.c pg_backup_archiver.c pg_backup_custom.c \
                   pg_backup_db.c pg_backup_files.c pg_backup_null.c \
                   pg_backup_tar.c pg_restore.c pg_dumpall.c \
                   ../../port/exec.c
GETTEXT_TRIGGERS:= write_msg:2 die_horribly:3 exit_horribly:3 simple_prompt \
                   ExecuteSqlCommand:3 ahlog:3 _
