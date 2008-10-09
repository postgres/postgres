# $PostgreSQL: pgsql/src/pl/tcl/nls.mk,v 1.1 2008/10/09 17:24:05 alvherre Exp $
CATALOG_NAME	:= pltcl
AVAIL_LANGUAGES	:=
GETTEXT_FILES	:= pltcl.c
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext write_stderr yyerror
