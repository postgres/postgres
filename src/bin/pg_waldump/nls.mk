# src/bin/pg_waldump/nls.mk
CATALOG_NAME     = pg_waldump
AVAIL_LANGUAGES  = de fr ko ru sv tr
GETTEXT_FILES    = pg_waldump.c
GETTEXT_TRIGGERS = fatal_error
GETTEXT_FLAGS    = fatal_error:1:c-format
