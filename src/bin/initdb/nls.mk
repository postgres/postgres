# src/bin/initdb/nls.mk
CATALOG_NAME     = initdb
AVAIL_LANGUAGES  = cs de es fr it ja ko pl pt_BR ro ru sv ta tr zh_CN zh_TW
GETTEXT_FILES    = findtimezone.c initdb.c ../../common/fe_memutils.c ../../port/dirmod.c ../../port/exec.c ../../port/wait_error.c
GETTEXT_TRIGGERS = simple_prompt
