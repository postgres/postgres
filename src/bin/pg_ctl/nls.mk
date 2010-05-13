# $PostgreSQL: pgsql/src/bin/pg_ctl/nls.mk,v 1.18.2.2 2010/05/13 10:50:02 petere Exp $
CATALOG_NAME	:= pg_ctl
AVAIL_LANGUAGES	:= de es fr it ja ko pt_BR ru sv ta tr zh_CN
GETTEXT_FILES	:= pg_ctl.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
