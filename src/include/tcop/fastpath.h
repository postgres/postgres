/*-------------------------------------------------------------------------
 *
 * fastpath.h
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/fastpath.h,v 1.18 2004/12/31 22:03:44 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FASTPATH_H
#define FASTPATH_H

#include "lib/stringinfo.h"

extern int	HandleFunctionRequest(StringInfo msgBuf);

#endif   /* FASTPATH_H */
