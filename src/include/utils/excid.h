/*-------------------------------------------------------------------------
 *
 * excid.h
 *	  POSTGRES known exception identifier definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: excid.h,v 1.10 2001/03/23 18:26:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXCID_H
#define EXCID_H

/* note: these first three are also declared in postgres.h */
extern DLLIMPORT Exception FailedAssertion;
extern DLLIMPORT Exception BadState;
extern DLLIMPORT Exception BadArg;
extern DLLIMPORT Exception Unimplemented;

extern DLLIMPORT Exception CatalogFailure;/* XXX inconsistent naming style */
extern DLLIMPORT Exception InternalError; /* XXX inconsistent naming style */
extern DLLIMPORT Exception SemanticError; /* XXX inconsistent naming style */
extern DLLIMPORT Exception SystemError;	/* XXX inconsistent naming style */

#endif	 /* EXCID_H */
