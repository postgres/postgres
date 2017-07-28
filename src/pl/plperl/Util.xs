/**********************************************************************
 * PostgreSQL::InServer::Util
 *
 * src/pl/plperl/Util.xs
 *
 * Defines plperl interfaces for general-purpose utilities.
 * This module is bootstrapped as soon as an interpreter is initialized.
 * Currently doesn't define a PACKAGE= so all subs are in main:: to avoid
 * the need for explicit importing.
 *
 **********************************************************************/

/* this must be first: */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/bytea.h"       /* for byteain & byteaout */

/* Defined by Perl */
#undef _

/* perl stuff */
#define PG_NEED_PERL_XSUB_H
#include "plperl.h"
#include "plperl_helpers.h"


static text *
sv2text(SV *sv)
{
	char	   *str = sv2cstr(sv);
	text	   *text;

	text = cstring_to_text(str);
	pfree(str);
	return text;
}

MODULE = PostgreSQL::InServer::Util PREFIX = util_

PROTOTYPES: ENABLE
VERSIONCHECK: DISABLE

int
_aliased_constants()
    PROTOTYPE:
    ALIAS:
        DEBUG   = DEBUG2
        LOG     = LOG
        INFO    = INFO
        NOTICE  = NOTICE
        WARNING = WARNING
        ERROR   = ERROR
    CODE:
    /* uses the ALIAS value as the return value */
    RETVAL = ix;
    OUTPUT:
    RETVAL


void
util_elog(level, msg)
    int level
    SV *msg
    CODE:
        if (level > ERROR)      /* no PANIC allowed thanks */
            level = ERROR;
        if (level < DEBUG5)
            level = DEBUG5;
        plperl_util_elog(level, msg);

SV *
util_quote_literal(sv)
    SV *sv
    CODE:
    if (!sv || !SvOK(sv)) {
        RETVAL = &PL_sv_undef;
    }
    else {
        text *arg = sv2text(sv);
		text *quoted = DatumGetTextPP(DirectFunctionCall1(quote_literal, PointerGetDatum(arg)));
		char *str;

		pfree(arg);
		str = text_to_cstring(quoted);
		RETVAL = cstr2sv(str);
		pfree(str);
    }
    OUTPUT:
    RETVAL

SV *
util_quote_nullable(sv)
    SV *sv
    CODE:
    if (!sv || !SvOK(sv))
	{
        RETVAL = cstr2sv("NULL");
    }
    else
	{
        text *arg = sv2text(sv);
		text *quoted = DatumGetTextPP(DirectFunctionCall1(quote_nullable, PointerGetDatum(arg)));
		char *str;

		pfree(arg);
		str = text_to_cstring(quoted);
		RETVAL = cstr2sv(str);
		pfree(str);
    }
    OUTPUT:
    RETVAL

SV *
util_quote_ident(sv)
    SV *sv
    PREINIT:
        text *arg;
		text *quoted;
		char *str;
    CODE:
        arg = sv2text(sv);
		quoted = DatumGetTextPP(DirectFunctionCall1(quote_ident, PointerGetDatum(arg)));

		pfree(arg);
		str = text_to_cstring(quoted);
		RETVAL = cstr2sv(str);
		pfree(str);
    OUTPUT:
    RETVAL

SV *
util_decode_bytea(sv)
    SV *sv
    PREINIT:
        char *arg;
        text *ret;
    CODE:
        arg = SvPVbyte_nolen(sv);
        ret = DatumGetTextPP(DirectFunctionCall1(byteain, PointerGetDatum(arg)));
        /* not cstr2sv because this is raw bytes not utf8'able */
        RETVAL = newSVpvn(VARDATA_ANY(ret), VARSIZE_ANY_EXHDR(ret));
    OUTPUT:
    RETVAL

SV *
util_encode_bytea(sv)
    SV *sv
    PREINIT:
        text *arg;
        char *ret;
		STRLEN len;
    CODE:
        /* not sv2text because this is raw bytes not utf8'able */
        ret = SvPVbyte(sv, len);
		arg = cstring_to_text_with_len(ret, len);
        ret = DatumGetCString(DirectFunctionCall1(byteaout, PointerGetDatum(arg)));
        RETVAL = cstr2sv(ret);
    OUTPUT:
    RETVAL

SV *
looks_like_number(sv)
    SV *sv
    CODE:
    if (!SvOK(sv))
        RETVAL = &PL_sv_undef;
    else if ( looks_like_number(sv) )
        RETVAL = &PL_sv_yes;
    else
        RETVAL = &PL_sv_no;
    OUTPUT:
    RETVAL

SV *
encode_typed_literal(sv, typname)
	SV 	   *sv
	char   *typname;
	PREINIT:
		char 	*outstr;
	CODE:
		outstr = plperl_sv_to_literal(sv, typname);
		if (outstr == NULL)
			RETVAL = &PL_sv_undef;
		else
			RETVAL = cstr2sv(outstr);
	OUTPUT:
	RETVAL

BOOT:
    items = 0;  /* avoid 'unused variable' warning */
