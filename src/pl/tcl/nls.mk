# $PostgreSQL: pgsql/src/pl/tcl/nls.mk,v 1.7 2009/10/20 18:23:27 petere Exp $
CATALOG_NAME	:= pltcl
AVAIL_LANGUAGES	:= de es fr it ja pt_BR tr
GETTEXT_FILES	:= pltcl.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
