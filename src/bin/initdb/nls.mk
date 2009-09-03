# $PostgreSQL: pgsql/src/bin/initdb/nls.mk,v 1.21.2.1 2009/09/03 21:01:00 petere Exp $
CATALOG_NAME	:= initdb
AVAIL_LANGUAGES	:= cs de es fr it ja pt_BR ru sv ta tr
GETTEXT_FILES	:= initdb.c ../../port/dirmod.c ../../port/exec.c
GETTEXT_TRIGGERS:= _ simple_prompt
