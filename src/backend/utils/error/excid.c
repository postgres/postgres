/*-------------------------------------------------------------------------
 *
 * excid.c--
 *	  POSTGRES known exception identifier code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/error/Attic/excid.c,v 1.3 1997/09/07 04:53:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/excid.h"

/*****************************************************************************
 *	 Generic Recoverable Exceptions											 *
 *****************************************************************************/


/*
 * FailedAssertion --
 *		Indicates an Assert(...) failed.
 */
Exception		FailedAssertion = {"Failed Assertion"};

/*
 * BadState --
 *		Indicates a function call request is inconsistent with module state.
 */
Exception		BadState = {"Bad State for Function Call"};

/*
 * BadArg --
 *		Indicates a function call argument or arguments is out-of-bounds.
 */
Exception		BadArg = {"Bad Argument to Function Call"};

/*****************************************************************************
 *	 Specific Recoverable Exceptions										 *
 *****************************************************************************/

/*
 * BadAllocSize --
 *		Indicates that an allocation request is of unreasonable size.
 */
Exception		BadAllocSize = {"Too Large Allocation Request"};

/*
 * ExhaustedMemory --
 *		Indicates an dynamic memory allocation failed.
 */
Exception		ExhaustedMemory = {"Memory Allocation Failed"};

/*
 * Unimplemented --
 *		Indicates a function call request requires unimplemented code.
 */
Exception		Unimplemented = {"Unimplemented Functionality"};

Exception		CatalogFailure = {"Catalog failure"};	/* XXX inconsistent */
Exception		InternalError = {"Internal Error"};		/* XXX inconsistent */
Exception		SemanticError = {"Semantic Error"};		/* XXX inconsistent */
Exception		SystemError = {"System Error"}; /* XXX inconsistent */
