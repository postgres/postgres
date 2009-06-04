# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.6 2009/06/04 18:33:07 tgl Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= de es fr pt_BR tr
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
