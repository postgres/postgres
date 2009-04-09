# $PostgreSQL: pgsql/src/pl/plpython/nls.mk,v 1.3 2009/04/09 19:38:53 petere Exp $
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:= de es fr
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errdetail errdetail_log errhint errcontext PLy_elog:2 PLy_exception_set:2
