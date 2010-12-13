# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.23 2010/05/13 15:56:37 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ja ko pt_BR ro ru sv ta tr zh_CN zh_TW
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
