/*-------------------------------------------------------------------------
 *
 * excid.h
 *	  POSTGRES known exception identifier definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: excid.h,v 1.8 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXCID_H
#define EXCID_H


extern Exception FailedAssertion;
extern Exception BadState;
extern Exception BadArg;
extern Exception BadAllocSize;
extern Exception ExhaustedMemory;
extern Exception Unimplemented;

extern Exception CatalogFailure;/* XXX inconsistent naming style */
extern Exception InternalError; /* XXX inconsistent naming style */
extern Exception SemanticError; /* XXX inconsistent naming style */
extern Exception SystemError;	/* XXX inconsistent naming style */

#endif	 /* EXCID_H */
