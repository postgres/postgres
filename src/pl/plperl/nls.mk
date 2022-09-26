# src/pl/plperl/nls.mk
CATALOG_NAME     = plperl
AVAIL_LANGUAGES  = cs de el es fr it ja ka ko pl pt_BR ru sv tr uk vi zh_CN
GETTEXT_FILES    = plperl.c SPI.c
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS)
