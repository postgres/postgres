# $PostgreSQL: pgsql/src/pl/tcl/nls.mk,v 1.6.2.2 2010/05/13 10:50:20 petere Exp $
CATALOG_NAME	:= pltcl
AVAIL_LANGUAGES	:= de es fr it ja pt_BR tr zh_CN
GETTEXT_FILES	:= pltcl.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
