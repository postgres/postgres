/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl.h
 */

#ifndef PL_PERL_H
#define PL_PERL_H

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

#endif							/* PL_PERL_H */
