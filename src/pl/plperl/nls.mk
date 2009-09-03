# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.7.2.1 2009/09/03 21:01:21 petere Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= de es fr it ja pt_BR tr
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
