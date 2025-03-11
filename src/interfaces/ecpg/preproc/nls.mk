# src/interfaces/ecpg/preproc/nls.mk
CATALOG_NAME     = ecpg
GETTEXT_FILES    = descriptor.c ecpg.c pgc.c preproc.c type.c util.c variable.c
GETTEXT_TRIGGERS = mmerror:3 mmfatal:2
GETTEXT_FLAGS    = mmerror:3:c-format mmfatal:2:c-format
