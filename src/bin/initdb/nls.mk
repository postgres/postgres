# src/bin/initdb/nls.mk
CATALOG_NAME     = initdb
AVAIL_LANGUAGES  = cs de es fr it ja pl pt_BR ru zh_CN
GETTEXT_FILES    = findtimezone.c initdb.c ../../common/fe_memutils.c ../../port/dirmod.c ../../port/exec.c ../../port/wait_error.c
GETTEXT_TRIGGERS = simple_prompt
