# $PostgreSQL: pgsql/src/bin/pg_ctl/nls.mk,v 1.19 2009/10/20 18:23:24 petere Exp $
CATALOG_NAME	:= pg_ctl
AVAIL_LANGUAGES	:= de es fr it ja ko pt_BR ru sv ta tr
GETTEXT_FILES	:= pg_ctl.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
