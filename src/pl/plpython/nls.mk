# $PostgreSQL: pgsql/src/pl/plpython/nls.mk,v 1.7 2009/10/20 18:23:27 petere Exp $
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:= de es fr it ja pt_BR tr
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext PLy_elog:2 PLy_exception_set:2 PLy_exception_set_plural:2,3
