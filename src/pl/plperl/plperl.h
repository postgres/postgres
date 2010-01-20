/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/pl/plperl/plperl.h,v 1.11 2010/01/20 01:08:21 adunstan Exp $
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

/* required for perl API */
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

/* perl version and platform portability */
#define NEED_eval_pv
#define NEED_newRV_noinc
#define NEED_sv_2pv_flags
#include "ppport.h"

/* perl may have a different width of "bool", don't buy it */
#ifdef bool
#undef bool
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



#endif   /* PL_PERL_H */
