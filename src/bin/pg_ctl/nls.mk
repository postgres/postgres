# $PostgreSQL: pgsql/src/bin/pg_ctl/nls.mk,v 1.18.2.1 2009/09/03 21:01:07 petere Exp $
CATALOG_NAME	:= pg_ctl
AVAIL_LANGUAGES	:= de es fr it ja ko pt_BR ru sv ta tr
GETTEXT_FILES	:= pg_ctl.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
