# $Header: /cvsroot/pgsql/src/interfaces/libpq/nls.mk,v 1.11 2003/10/03 22:39:34 petere Exp $
CATALOG_NAME	:= libpq
AVAIL_LANGUAGES	:= cs de es fr hr nb pt_BR ru sv zh_CN zh_TW
GETTEXT_FILES	:= fe-auth.c fe-connect.c fe-exec.c fe-lobj.c fe-misc.c fe-secure.c
GETTEXT_TRIGGERS:= libpq_gettext pqInternalNotice:2
