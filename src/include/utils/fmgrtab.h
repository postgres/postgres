/*-------------------------------------------------------------------------
 *
 * fmgrtab.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgrtab.h,v 1.11 1999/04/09 22:35:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H


typedef struct
{
	Oid			proid;
	int			nargs;
	func_ptr	func;
	char	   *funcName;
} FmgrCall;

extern FmgrCall *fmgr_isbuiltin(Oid id);
extern func_ptr fmgr_lookupByName(char *name);
extern void load_file(char *filename);

#endif	 /* FMGRTAB_H */
