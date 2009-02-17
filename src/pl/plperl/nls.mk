# $PostgreSQL: pgsql/src/pl/plperl/nls.mk,v 1.2 2009/02/17 09:24:57 petere Exp $
CATALOG_NAME	:= plperl
AVAIL_LANGUAGES	:=
GETTEXT_FILES	:= plperl.c SPI.xs
GETTEXT_TRIGGERS:= _ errmsg errdetail errdetail_log errhint errcontext croak Perl_croak
