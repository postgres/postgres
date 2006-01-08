/*-------------------------------------------------------------------------
 *
 * plperl.h
 *    Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/pl/plperl/plperl.h,v 1.1 2006/01/08 22:27:52 adunstan Exp $
 */

#ifndef PL_PERL_H
#define PL_PERL_H

/* stop perl headers from hijacking stdio and other stuff on Windows */
#ifdef WIN32
#define WIN32IO_IS_STDIO
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


#endif /* PL_PERL_H */
