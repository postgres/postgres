# src/bin/initdb/nls.mk
CATALOG_NAME     = initdb
AVAIL_LANGUAGES  = cs de es fr it ja pl pt_BR ru sv zh_CN
GETTEXT_FILES    = findtimezone.c initdb.c ../../common/exec.c ../../common/fe_memutils.c ../../common/pgfnames.c ../../common/rmtree.c ../../common/username.c ../../common/wait_error.c ../../port/dirmod.c
GETTEXT_TRIGGERS = simple_prompt
