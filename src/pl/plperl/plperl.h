/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files, as
 * well as headers that could in turn include system headers.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl.h
 */

#ifndef PL_PERL_H
#define PL_PERL_H

/* defines free() by way of system headers, so must be included before perl.h */
#include "mb/pg_wchar.h"

/* stop perl headers from hijacking stdio and other stuff on Windows */
#ifdef WIN32
#define WIN32IO_IS_STDIO
#endif							/* WIN32 */

/*
 * Supply a value of PERL_UNUSED_DECL that will satisfy gcc - the one
 * perl itself supplies doesn't seem to.
 */
#define PERL_UNUSED_DECL pg_attribute_unused()

/*
 * Sometimes perl carefully scribbles on our *printf macros.
 * So we undefine them here and redefine them after it's done its dirty deed.
 */
#undef vsnprintf
#undef snprintf
#undef vsprintf
#undef sprintf
#undef vfprintf
#undef fprintf
#undef vprintf
#undef printf

/*
 * Perl scribbles on the "_" macro too.
 */
#undef _

/*
 * ActivePerl 5.18 and later are MinGW-built, and their headers use GCC's
 * __inline__.  Translate to something MSVC recognizes. Also, perl.h sometimes
 * defines isnan, so undefine it here and put back the definition later if
 * perl.h doesn't.
 */
#ifdef _MSC_VER
#define __inline__ inline
#ifdef isnan
#undef isnan
#endif
/* Work around for using MSVC and Strawberry Perl >= 5.30. */
#define __builtin_expect(expr, val) (expr)
#endif

/*
 * Regarding bool, both PostgreSQL and Perl might use stdbool.h or not,
 * depending on configuration.  If both agree, things are relatively harmless.
 * If not, things get tricky.  If PostgreSQL does but Perl does not, define
 * HAS_BOOL here so that Perl does not redefine bool; this avoids compiler
 * warnings.  If PostgreSQL does not but Perl does, we need to undefine bool
 * after we include the Perl headers; see below.
 */
#ifdef PG_USE_STDBOOL
#define HAS_BOOL 1
#endif

/*
 * Newer versions of the perl headers trigger a lot of warnings with our
 * compiler flags (at least -Wdeclaration-after-statement,
 * -Wshadow=compatible-local are known to be problematic). The system_header
 * pragma hides warnings from within the rest of this file, if supported.
 */
#ifdef HAVE_PRAGMA_GCC_SYSTEM_HEADER
#pragma GCC system_header
#endif

/*
 * Get the basic Perl API.  We use PERL_NO_GET_CONTEXT mode so that our code
 * can compile against MULTIPLICITY Perl builds without including XSUB.h.
 */
#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"

/*
 * We want to include XSUB.h only within .xs files, because on some platforms
 * it undesirably redefines a lot of libc functions.  But it must appear
 * before ppport.h, so use a #define flag to control inclusion here.
 */
#ifdef PG_NEED_PERL_XSUB_H
/*
 * On Windows, win32_port.h defines macros for a lot of these same functions.
 * To avoid compiler warnings when XSUB.h redefines them, #undef our versions.
 */
#ifdef WIN32
#undef accept
#undef bind
#undef connect
#undef fopen
#undef fstat
#undef kill
#undef listen
#undef lstat
#undef mkdir
#undef open
#undef putenv
#undef recv
#undef rename
#undef select
#undef send
#undef socket
#undef stat
#undef unlink
#endif

#include "XSUB.h"
#endif

/* put back our *printf macros ... this must match src/include/port.h */
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsprintf
#undef vsprintf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef vfprintf
#undef vfprintf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef vprintf
#undef vprintf
#endif
#ifdef printf
#undef printf
#endif

#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#define vsprintf		pg_vsprintf
#define sprintf			pg_sprintf
#define vfprintf		pg_vfprintf
#define fprintf			pg_fprintf
#define vprintf			pg_vprintf
#define printf(...)		pg_printf(__VA_ARGS__)

/*
 * Put back "_" too; but rather than making it just gettext() as the core
 * code does, make it dgettext() so that the right things will happen in
 * loadable modules (if they've set up TEXTDOMAIN correctly).  Note that
 * we can't just set TEXTDOMAIN here, because this file is used by more
 * extensions than just PL/Perl itself.
 */
#undef _
#define _(x) dgettext(TEXTDOMAIN, x)

/* put back the definition of isnan if needed */
#ifdef _MSC_VER
#ifndef isnan
#define isnan(x) _isnan(x)
#endif
#endif

/* perl version and platform portability */
#include "ppport.h"

/*
 * perl might have included stdbool.h.  If we also did that earlier (see c.h),
 * then that's fine.  If not, we probably rejected it for some reason.  In
 * that case, undef bool and proceed with our own bool.  (Note that stdbool.h
 * makes bool a macro, but our own replacement is a typedef, so the undef
 * makes ours visible again).
 */
#ifndef PG_USE_STDBOOL
#ifdef bool
#undef bool
#endif
#endif

/* supply HeUTF8 if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef HeUTF8
#define HeUTF8(he)			   ((HeKLEN(he) == HEf_SVKEY) ?			   \
								SvUTF8(HeKEY_sv(he)) :				   \
								(U32)HeKUTF8(he))
#endif

/* supply GvCV_set if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef GvCV_set
#define GvCV_set(gv, cv)		(GvCV(gv) = cv)
#endif

/* Perl 5.19.4 changed array indices from I32 to SSize_t */
#if PERL_BCDVERSION >= 0x5019004
#define AV_SIZE_MAX SSize_t_MAX
#else
#define AV_SIZE_MAX I32_MAX
#endif

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
