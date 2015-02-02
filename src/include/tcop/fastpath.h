/*-------------------------------------------------------------------------
 *
 * fastpath.h
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/fastpath.h,v 1.23 2010/01/02 16:58:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FASTPATH_H
#define FASTPATH_H

#include "lib/stringinfo.h"

extern int GetOldFunctionMessage(StringInfo buf);
extern int	HandleFunctionRequest(StringInfo msgBuf);

#endif   /* FASTPATH_H */
