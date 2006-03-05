/*-------------------------------------------------------------------------
 *
 * dl.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/port/dynloader/ultrix4.h,v 1.16 2006/03/05 15:58:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	Ultrix 4.x Dynamic Loader Library Version 1.0
 *
 *	dl.h
 *		header file for the Dynamic Loader Library
 *
 *
 *	Copyright (c) 1993 Andrew K. Yu, University of California at Berkeley
 *	All rights reserved.
 *
 *	Permission to use, copy, modify, and distribute this software and its
 *	documentation for educational, research, and non-profit purposes and
 *	without fee is hereby granted, provided that the above copyright
 *	notice appear in all copies and that both that copyright notice and
 *	this permission notice appear in supporting documentation. Permission
 *	to incorporate this software into commercial products can be obtained
 *	from the author. The University of California and the author make
 *	no representations about the suitability of this software for any
 *	purpose. It is provided "as is" without express or implied warranty.
 *
 */
#ifndef _DL_HEADER_
#define _DL_HEADER_

#include <stdio.h>
#include "filehdr.h"
#include "syms.h"
#include "ldfcn.h"
#include "reloc.h"
#include "scnhdr.h"


typedef long CoreAddr;


typedef struct ScnInfo
{
	CoreAddr	addr;			/* starting address of the section */
	SCNHDR		hdr;			/* section header */
	RELOC	   *relocEntries;	/* relocation entries */
}	ScnInfo;

typedef enum
{
	DL_NEEDRELOC,				/* still need relocation */
	DL_RELOCATED,				/* no relocation necessary */
	DL_INPROG					/* relocation in progress */
}	dlRStatus;

typedef struct JmpTbl
{
	char	   *block;			/* the jump table memory block */
	struct JmpTbl *next;		/* next block */
}	JmpTbl;

typedef struct dlFile
{
	char	   *filename;		/* file name of the object file */

	int			textSize;		/* used by mprotect */
	CoreAddr	textAddress;	/* start addr of text section */
	long		textVaddr;		/* vaddr of text section in obj file */
	CoreAddr	rdataAddress;	/* start addr of rdata section */
	long		rdataVaddr;		/* vaddr of text section in obj file */
	CoreAddr	dataAddress;	/* start addr of data section */
	long		dataVaddr;		/* vaddr of text section in obj file */
	CoreAddr	bssAddress;		/* start addr of bss section */
	long		bssVaddr;		/* vaddr of text section in obj file */

	int			nsect;			/* number of sections */
	ScnInfo    *sect;			/* details of each section (array) */

	int			issExtMax;		/* size of string space */
	char	   *extss;			/* extern sym string space (in core) */
	int			iextMax;		/* maximum number of Symbols */
	pEXTR		extsyms;		/* extern syms */

	dlRStatus	relocStatus;	/* what relocation needed? */
	int			needReloc;

	JmpTbl	   *jmptable;		/* the jump table for R_JMPADDR */

	struct dlFile *next;		/* next member of the archive */
}	dlFile;

typedef struct dlSymbol
{
	char	   *name;			/* name of the symbol */
	long		addr;			/* address of the symbol */
	dlFile	   *objFile;		/* from which file */
}	dlSymbol;

/*
 * prototypes for the dl* interface
 */
extern void *dl_open( /* char *filename, int mode */ );
extern void *dl_sym( /* void *handle, char *name */ );
extern void dl_close( /* void *handle */ );
extern char *dl_error( /* void */ );

#define   DL_LAZY		0		/* lazy resolution */
#define   DL_NOW		1		/* immediate resolution */

/*
 * Miscellaneous utility routines:
 */
extern char **dl_undefinedSymbols( /* int *count */ );
extern void dl_printAllSymbols( /* void *handle */ );
extern void dl_setLibraries( /* char *libs */ );

#endif   /* _DL_HEADER_ */
