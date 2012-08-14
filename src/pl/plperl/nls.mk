# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.7.2.2 2010/05/13 10:50:17 petere Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:= de es fr it ja pl pt_BR ru tr zh_CN
GETTEXT_FILES	:= plperl.c SPI.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext
