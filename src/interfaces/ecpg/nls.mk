# $PostgreSQL $
CATALOG_NAME	= ecpg
AVAIL_LANGUAGES	=
GETTEXT_FILES	= \
	compatlib/informix.c \
	ecpglib/connect.c \
	ecpglib/data.c \
	ecpglib/descriptor.c \
	ecpglib/error.c \
	ecpglib/execute.c \
	ecpglib/misc.c \
	ecpglib/prepare.c \
	include/ecpglib.h \
	preproc/descriptor.c \
	preproc/ecpg.c \
	preproc/pgc.c \
	preproc/preproc.c \
	preproc/type.c \
	preproc/variable.c
GETTEXT_TRIGGERS = _ mmerror:3 ecpg_gettext ecpg_log:1


# Without these rules, make will invoke predefined implicit rules using "lex" 
# and "yacc", which we don't want.  The prerequisites should be exactly the 
# same as in preproc/Makefile.

$(srcdir)/preproc/preproc.c: preproc/preproc.y
	$(MAKE) -C preproc preproc.c

$(srcdir)/preproc/pgc.c: preproc/pgc.l
	$(MAKE) -C preproc pgc.c
