# src/bin/pg_basebackup/nls.mk
CATALOG_NAME     = pg_basebackup
AVAIL_LANGUAGES  = de es fr it ko pl pt_BR ru zh_CN
GETTEXT_FILES    = pg_basebackup.c pg_receivexlog.c pg_recvlogical.c receivelog.c streamutil.c ../../common/fe_memutils.c
GETTEXT_TRIGGERS = simple_prompt
