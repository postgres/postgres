/*-------------------------------------------------------------------------
 *
 * funcindex.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: funcindex.h,v 1.1.1.1 1996/07/09 06:21:08 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _FUNC_INDEX_INCLUDED_
#define _FUNC_INDEX_INCLUDED_

#include "postgres.h"

typedef struct {
	int	nargs;
	Oid	arglist[8];
	Oid	procOid;
	NameData funcName;
} FuncIndexInfo;

typedef FuncIndexInfo	*FuncIndexInfoPtr;

/*
 * some marginally useful macro definitions
 */
/* #define FIgetname(FINFO) (&((FINFO)->funcName.data[0]))*/
#define FIgetname(FINFO) (FINFO)->funcName.data
#define FIgetnArgs(FINFO) (FINFO)->nargs
#define FIgetProcOid(FINFO) (FINFO)->procOid
#define FIgetArg(FINFO, argnum) (FINFO)->arglist[argnum]
#define FIgetArglist(FINFO) (FINFO)->arglist

#define FIsetnArgs(FINFO, numargs) ((FINFO)->nargs = numargs)
#define FIsetProcOid(FINFO, id) ((FINFO)->procOid = id)
#define FIsetArg(FINFO, argnum, argtype) ((FINFO)->arglist[argnum] = argtype)

#define FIisFunctionalIndex(FINFO) (FINFO->procOid != InvalidOid)

#endif /* FUNCINDEX_H */
