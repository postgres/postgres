# src/interfaces/libpq/nls.mk
CATALOG_NAME     = libpq
GETTEXT_FILES    = $(wildcard *.c) ../../port/thread.c
GETTEXT_TRIGGERS = libpq_gettext pqInternalNotice:2
GETTEXT_FLAGS    = libpq_gettext:1:pass-c-format pqInternalNotice:2:c-format
