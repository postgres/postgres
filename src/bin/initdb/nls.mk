# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.21 2009/06/26 19:33:49 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr ja pt_BR ru sv ta tr
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
