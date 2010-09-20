# src/bin/initdb/nls.mk
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ja pt_BR ru sv ta tr zh_CN
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
