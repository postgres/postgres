# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.4 2009/04/09 19:38:53 petere Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= de es fr
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errdetail errdetail_log errhint errcontext
