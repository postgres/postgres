# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.8.10.1 2010/09/16 19:09:38 petere Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= cs de es fr it ja ko pl pt_BR ro ru tr zh_CN zh_TW
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
