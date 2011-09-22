# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.21.2.2 2010/05/13 10:50:00 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ja ko pl pt_BR ru sv ta tr zh_CN zh_TW
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
