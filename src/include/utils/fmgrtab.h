/*-------------------------------------------------------------------------
 *
 * fmgrtab.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fmgrtab.h,v 1.10 1999/03/29 01:30:41 tgl Exp $
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
	int			dummy;			/* pad struct to 4 words for fast indexing */
} FmgrCall;

extern FmgrCall *fmgr_isbuiltin(Oid id);

extern void load_file(char *filename);

#endif	 /* FMGRTAB_H */
