# $Header: /cvsroot/pgsql/src/interfaces/libpq/nls.mk,v 1.13.2.1 2004/10/30 08:22:16 petere Exp $
CATALOG_NAME	:= libpq
AVAIL_LANGUAGES	:= cs de es fr hr it nb pt_BR ru sl sv tr zh_CN zh_TW
GETTEXT_FILES	:= fe-auth.c fe-connect.c fe-exec.c fe-lobj.c fe-misc.c fe-secure.c
GETTEXT_TRIGGERS:= libpq_gettext pqInternalNotice:2
