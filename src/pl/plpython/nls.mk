# $PostgreSQL: pgsql/src/pl/plpython/nls.mk,v 1.4 2009/05/14 21:41:53 alvherre Exp $
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:= de es fr pt_BR tr
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errdetail errdetail_log errhint errcontext PLy_elog:2 PLy_exception_set:2
