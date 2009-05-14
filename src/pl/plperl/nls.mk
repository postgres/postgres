# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.5 2009/05/14 21:41:53 alvherre Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= de es fr pt_BR tr
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errdetail errdetail_log errhint errcontext
