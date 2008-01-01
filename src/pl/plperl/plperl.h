/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/pl/plperl/plperl.h,v 1.8 2008/01/01 19:46:00 momjian Exp $
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
#include "ppport.h"

/* just in case these symbols aren't provided */
#ifndef pTHX_
#define pTHX_
#define pTHX void
#endif

/* perl may have a different width of "bool", don't buy it */
#ifdef bool
#undef bool
#endif

/* routines from spi_internal.c */
int			spi_DEBUG(void);
int			spi_LOG(void);
int			spi_INFO(void);
int			spi_NOTICE(void);
int			spi_WARNING(void);
int			spi_ERROR(void);

/* routines from plperl.c */
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
