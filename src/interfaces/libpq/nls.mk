# $Header: /cvsroot/pgsql/src/interfaces/libpq/nls.mk,v 1.10 2003/09/29 10:57:06 petere Exp $
CATALOG_NAME	:= libpq
AVAIL_LANGUAGES	:= cs de es fr nb pt_BR ru sv zh_CN zh_TW
GETTEXT_FILES	:= fe-auth.c fe-connect.c fe-exec.c fe-lobj.c fe-misc.c fe-secure.c
GETTEXT_TRIGGERS:= libpq_gettext pqInternalNotice:2
