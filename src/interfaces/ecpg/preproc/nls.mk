# src/interfaces/ecpg/preproc/nls.mk
CATALOG_NAME     = ecpg
GETTEXT_FILES    = $(wildcard *.c)
GETTEXT_TRIGGERS = mmerror:3 mmfatal:2
GETTEXT_FLAGS    = mmerror:3:c-format mmfatal:2:c-format
