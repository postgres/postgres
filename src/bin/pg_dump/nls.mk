# $Header: /cvsroot/pgsql/src/bin/pg_dump/nls.mk,v 1.11.2.1 2004/04/15 08:13:00 petere Exp $
CATALOG_NAME	:= pg_dump
AVAIL_LANGUAGES	:= cs de es fr it nb pt_BR ru sv zh_CN zh_TW
GETTEXT_FILES	:= pg_dump.c common.c pg_backup_archiver.c pg_backup_custom.c \
                   pg_backup_db.c pg_backup_files.c pg_backup_null.c \
                   pg_backup_tar.c pg_restore.c pg_dumpall.c
GETTEXT_TRIGGERS:= write_msg:2 die_horribly:3 exit_horribly:3 simple_prompt \
                   ExecuteSqlCommand:3 ahlog:3 _
