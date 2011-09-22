# $PostgreSQL: pgsql/src/interfaces/libpq/nls.mk,v 1.23.2.2 2010/05/13 10:50:13 petere Exp $
CATALOG_NAME	:= libpq
AVAIL_LANGUAGES	:= cs de es fr it ja ko pl pt_BR ru sv ta tr zh_CN zh_TW
GETTEXT_FILES	:= fe-auth.c fe-connect.c fe-exec.c fe-lobj.c fe-misc.c fe-protocol2.c fe-protocol3.c fe-secure.c
GETTEXT_TRIGGERS:= libpq_gettext pqInternalNotice:2
