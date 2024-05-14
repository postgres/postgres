# src/bin/pg_combinebackup/nls.mk
CATALOG_NAME     = pg_combinebackup
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   backup_label.c \
                   copy_file.c \
                   load_manifest.c \
                   pg_combinebackup.c \
                   reconstruct.c \
                   write_manifest.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
