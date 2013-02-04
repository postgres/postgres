# src/pl/plperl/nls.mk
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= cs de es fr it ja pl pt_BR ro ru tr zh_CN zh_TW
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
