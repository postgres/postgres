# src/pl/plperl/nls.mk
CATALOG_NAME     = plperl
AVAIL_LANGUAGES  = cs de es fr it ja ko pl pt_BR ro ru sv tr zh_CN zh_TW
GETTEXT_FILES    = plperl.c SPI.c
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS)
