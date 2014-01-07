/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl.h
 */

#ifndef PL_PERL_H
#define PL_PERL_H

/* stop perl headers from hijacking stdio and other stuff on Windows */
#ifdef WIN32
#define WIN32IO_IS_STDIO
/*
 * isnan is defined in both the perl and mingw headers. We don't use it,
 * so this just clears up the compile warning.
 */
#ifdef isnan
#undef isnan
#endif
#endif

/*
 * Supply a value of PERL_UNUSED_DECL that will satisfy gcc - the one
 * perl itself supplies doesn't seem to.
 */
#if defined(__GNUC__)
#define PERL_UNUSED_DECL __attribute__ ((unused))
#endif

/*
 * Sometimes perl carefully scribbles on our *printf macros.
 * So we undefine them here and redefine them after it's done its dirty deed.
 */

#ifdef USE_REPL_SNPRINTF
#undef snprintf
#undef vsnprintf
#endif


/* required for perl API */
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

/* put back our snprintf and vsnprintf */
#ifdef USE_REPL_SNPRINTF
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef __GNUC__
#define vsnprintf(...)	pg_vsnprintf(__VA_ARGS__)
#define snprintf(...)	pg_snprintf(__VA_ARGS__)
#else
#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#endif   /* __GNUC__ */
#endif   /* USE_REPL_SNPRINTF */

/* perl version and platform portability */
#define NEED_eval_pv
#define NEED_newRV_noinc
#define NEED_sv_2pv_flags
#include "ppport.h"

/* perl may have a different width of "bool", don't buy it */
#ifdef bool
#undef bool
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
char	   *plperl_sv_to_literal(SV *, char *);

#endif   /* PL_PERL_H */
