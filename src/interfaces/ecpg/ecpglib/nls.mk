# src/interfaces/ecpg/ecpglib/nls.mk
CATALOG_NAME     = ecpglib
AVAIL_LANGUAGES  = cs de el es fr it ja ka ko pl pt_BR ru sv tr uk vi zh_CN
GETTEXT_FILES    = connect.c descriptor.c error.c execute.c misc.c
GETTEXT_TRIGGERS = ecpg_gettext
GETTEXT_FLAGS    = ecpg_gettext:1:pass-c-format
