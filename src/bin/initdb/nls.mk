# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.20 2009/04/09 19:38:50 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ja ko pl pt_BR ro ru sk sl sv ta tr zh_CN zh_TW
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
