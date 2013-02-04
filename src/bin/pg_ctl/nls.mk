# src/bin/pg_ctl/nls.mk
CATALOG_NAME	:= pg_ctl
AVAIL_LANGUAGES	:= cs de es fr it ja pl pt_BR ru tr zh_CN zh_TW
GETTEXT_FILES	:= pg_ctl.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
