# $PostgreSQL: pgsql/src/pl/plpython/nls.mk,v 1.2 2009/01/15 13:49:56 petere Exp $
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:=
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errdetail errdetail_log errhint errcontext PLy_elog:2 PLy_exception_set:2
