# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.18 2005/01/09 17:38:18 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ko pl pt_BR ro ru sk sl sv tr zh_CN zh_TW
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
