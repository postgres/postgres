/*-------------------------------------------------------------------------
 *
 * fmgrtab.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgrtab.h,v 1.9 1999/02/13 23:22:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H


typedef struct
{
	Oid			proid;
	uint16		nargs;
	func_ptr	func;
	char	   *funcName;
} FmgrCall;

extern FmgrCall *fmgr_isbuiltin(Oid id);
extern func_ptr fmgr_lookupByName(char *name);
extern void load_file(char *filename);

#endif	 /* FMGRTAB_H */
