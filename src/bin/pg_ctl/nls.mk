# $PostgreSQL: pgsql/src/bin/pg_ctl/nls.mk,v 1.16 2007/11/15 20:38:15 petere Exp $
CATALOG_NAME	:= pg_ctl
AVAIL_LANGUAGES	:= cs de es fr ko pt_BR ro ru sk sl sv ta tr zh_CN zh_TW
GETTEXT_FILES	:= pg_ctl.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
