/*-------------------------------------------------------------------------
 *
 * fmgrtab.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgrtab.h,v 1.3 1996/11/04 11:51:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H


typedef struct {
    Oid		proid;
    uint16	nargs;
    func_ptr	func;
    char*       funcName;
} FmgrCall;

extern FmgrCall	*fmgr_isbuiltin(Oid id);
extern func_ptr fmgr_lookupByName(char* name);

#endif	/* FMGRTAB_H */
