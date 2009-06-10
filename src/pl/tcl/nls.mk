# $PostgreSQL: pgsql/src/pl/tcl/nls.mk,v 1.6 2009/06/10 23:42:44 petere Exp $
CATALOG_NAME	:= pltcl
AVAIL_LANGUAGES	:= de es fr ja pt_BR tr
GETTEXT_FILES	:= pltcl.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
