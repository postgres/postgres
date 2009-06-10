# $PostgreSQL: pgsql/src/pl/plpython/nls.mk,v 1.6 2009/06/10 23:42:44 petere Exp $
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:= de es fr ja pt_BR tr
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext PLy_elog:2 PLy_exception_set:2 PLy_exception_set_plural:2,3
