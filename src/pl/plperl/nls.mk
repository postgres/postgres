# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.1 2008/10/09 17:24:05 alvherre Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:=
GETTEXT_FILES	:= plperl.c SPI.xs
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext write_stderr croak Perl_croak
