/*-------------------------------------------------------------------------
 *
 * fmgrtab.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgrtab.h,v 1.1.1.1 1996/07/09 06:22:02 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H

#include "postgres.h"		/* for ObjectId */
#include "fmgr.h"		/* genearated by Gen_fmgrtab.sh */

typedef struct {
    Oid		proid;
    uint16	nargs;
    func_ptr	func;
    char*       funcName;
} FmgrCall;

extern FmgrCall	*fmgr_isbuiltin(Oid id);
extern func_ptr fmgr_lookupByName(char* name);

#endif	/* FMGRTAB_H */
