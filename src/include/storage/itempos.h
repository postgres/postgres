/*-------------------------------------------------------------------------
 *
 * itempos.h
 *	  Standard POSTGRES buffer page long item subposition definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: itempos.h,v 1.7 1999/02/13 23:22:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMPOS_H
#define ITEMPOS_H

#include <storage/itemid.h>
#include <storage/buf.h>

typedef struct ItemSubpositionData
{
	Buffer		op_db;
	ItemId		op_lpp;
	char	   *op_cp;			/* XXX */
	uint32		op_len;
}			ItemSubpositionData;

typedef ItemSubpositionData *ItemSubposition;

/*
 *		PNOBREAK(OBJP, LEN)
 *		struct	objpos	*OBJP;
 *		unsigned		LEN;
 */
#define PNOBREAK(OBJP, LEN)		((OBJP)->op_len >= LEN)

/*
 *		PSKIP(OBJP, LEN)
 *		struct	objpos	*OBJP;
 *		unsigned		LEN;
 */
#define PSKIP(OBJP, LEN)\
		{ (OBJP)->op_cp += (LEN); (OBJP)->op_len -= (LEN); }

#endif	 /* ITEMPOS_H */
