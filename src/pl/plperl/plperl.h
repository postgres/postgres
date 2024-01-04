/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files, as
 * well as headers that could in turn include system headers.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl.h
 */

#ifndef PL_PERL_H
#define PL_PERL_H

/* defines free() by way of system headers, so must be included before perl.h */
#include "mb/pg_wchar.h"

/*
 * Pull in Perl headers via a wrapper header, to control the scope of
 * the system_header pragma therein.
 */
#include "plperl_system.h"

/* declare routines from plperl.c for access by .xs files */
HV		   *plperl_spi_exec(char *, int);
void		plperl_return_next(SV *);
SV		   *plperl_spi_query(char *);
SV		   *plperl_spi_fetchrow(char *);
SV		   *plperl_spi_prepare(char *, int, SV **);
HV		   *plperl_spi_exec_prepared(char *, HV *, int, SV **);
SV		   *plperl_spi_query_prepared(char *, int, SV **);
void		plperl_spi_freeplan(char *);
void		plperl_spi_cursor_close(char *);
void		plperl_spi_commit(void);
void		plperl_spi_rollback(void);
char	   *plperl_sv_to_literal(SV *, char *);
void		plperl_util_elog(int level, SV *msg);


/* helper functions */

/*
 * convert from utf8 to database encoding
 *
 * Returns a palloc'ed copy of the original string
 */
static inline char *
utf_u2e(char *utf8_str, size_t len)
{
	char	   *ret;

	ret = pg_any_to_server(utf8_str, len, PG_UTF8);

	/* ensure we have a copy even if no conversion happened */
	if (ret == utf8_str)
		ret = pstrdup(ret);

	return ret;
}

/*
 * convert from database encoding to utf8
 *
 * Returns a palloc'ed copy of the original string
 */
static inline char *
utf_e2u(const char *str)
{
	char	   *ret;

	ret = pg_server_to_any(str, strlen(str), PG_UTF8);

	/* ensure we have a copy even if no conversion happened */
	if (ret == str)
		ret = pstrdup(ret);

	return ret;
}

/*
 * Convert an SV to a char * in the current database encoding
 *
 * Returns a palloc'ed copy of the original string
 */
static inline char *
sv2cstr(SV *sv)
{
	dTHX;
	char	   *val,
			   *res;
	STRLEN		len;

	/*
	 * get a utf8 encoded char * out of perl. *note* it may not be valid utf8!
	 */

	/*
	 * SvPVutf8() croaks nastily on certain things, like typeglobs and
	 * readonly objects such as $^V. That's a perl bug - it's not supposed to
	 * happen. To avoid crashing the backend, we make a copy of the sv before
	 * passing it to SvPVutf8(). The copy is garbage collected when we're done
	 * with it.
	 */
	if (SvREADONLY(sv) ||
		isGV_with_GP(sv) ||
		(SvTYPE(sv) > SVt_PVLV && SvTYPE(sv) != SVt_PVFM))
		sv = newSVsv(sv);
	else
	{
		/*
		 * increase the reference count so we can just SvREFCNT_dec() it when
		 * we are done
		 */
		SvREFCNT_inc_simple_void(sv);
	}

	/*
	 * Request the string from Perl, in UTF-8 encoding; but if we're in a
	 * SQL_ASCII database, just request the byte soup without trying to make
	 * it UTF8, because that might fail.
	 */
	if (GetDatabaseEncoding() == PG_SQL_ASCII)
		val = SvPV(sv, len);
	else
		val = SvPVutf8(sv, len);

	/*
	 * Now convert to database encoding.  We use perl's length in the event we
	 * had an embedded null byte to ensure we error out properly.
	 */
	res = utf_u2e(val, len);

	/* safe now to garbage collect the new SV */
	SvREFCNT_dec(sv);

	return res;
}

/*
 * Create a new SV from a string assumed to be in the current database's
 * encoding.
 */
static inline SV *
cstr2sv(const char *str)
{
	dTHX;
	SV		   *sv;
	char	   *utf8_str;

	/* no conversion when SQL_ASCII */
	if (GetDatabaseEncoding() == PG_SQL_ASCII)
		return newSVpv(str, 0);

	utf8_str = utf_e2u(str);

	sv = newSVpv(utf8_str, 0);
	SvUTF8_on(sv);
	pfree(utf8_str);

	return sv;
}

/*
 * croak() with specified message, which is given in the database encoding.
 *
 * Ideally we'd just write croak("%s", str), but plain croak() does not play
 * nice with non-ASCII data.  In modern Perl versions we can call cstr2sv()
 * and pass the result to croak_sv(); in versions that don't have croak_sv(),
 * we have to work harder.
 */
static inline void
croak_cstr(const char *str)
{
	dTHX;

#ifdef croak_sv
	/* Use sv_2mortal() to be sure the transient SV gets freed */
	croak_sv(sv_2mortal(cstr2sv(str)));
#else

	/*
	 * The older way to do this is to assign a UTF8-marked value to ERRSV and
	 * then call croak(NULL).  But if we leave it to croak() to append the
	 * error location, it does so too late (only after popping the stack) in
	 * some Perl versions.  Hence, use mess() to create an SV with the error
	 * location info already appended.
	 */
	SV		   *errsv = get_sv("@", GV_ADD);
	char	   *utf8_str = utf_e2u(str);
	SV		   *ssv;

	ssv = mess("%s", utf8_str);
	SvUTF8_on(ssv);

	pfree(utf8_str);

	sv_setsv(errsv, ssv);

	croak(NULL);
#endif							/* croak_sv */
}

#endif							/* PL_PERL_H */
