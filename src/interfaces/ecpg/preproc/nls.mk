# $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/nls.mk,v 1.7 2010/09/19 16:17:45 tgl Exp $
CATALOG_NAME	 = ecpg
AVAIL_LANGUAGES	 = de es fr it ja pt_BR tr zh_CN
GETTEXT_FILES	 = descriptor.c ecpg.c pgc.c preproc.c type.c variable.c
GETTEXT_TRIGGERS = _ mmerror:3
