/*-------------------------------------------------------------------------
 *
 * vacuum.h--
 *	  header file for postgres vacuum cleaner
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.8 1997/09/07 04:57:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include <access/funcindex.h>
#include <catalog/pg_index.h>

typedef struct VAttListData
{
	int				val_dummy;
	struct VAttListData *val_next;
}				VAttListData;

typedef VAttListData *VAttList;

typedef struct VPageDescrData
{
	BlockNumber		vpd_blkno;	/* BlockNumber of this Page */
	Size			vpd_free;	/* FreeSpace on this Page */
	uint16			vpd_nusd;	/* Number of OffNums used by vacuum */
	uint16			vpd_noff;	/* Number of OffNums free or to be free */
	OffsetNumber	vpd_voff[1];/* Array of its OffNums */
}				VPageDescrData;

typedef VPageDescrData *VPageDescr;

typedef struct VPageListData
{
	int				vpl_nemend; /* Number of "empty" end-pages */
	int				vpl_npages; /* Number of pages in vpl_pgdesc */
	VPageDescr	   *vpl_pgdesc; /* Descriptions of pages */
}				VPageListData;

typedef VPageListData *VPageList;

typedef struct
{
	FuncIndexInfo	finfo;
	FuncIndexInfo  *finfoP;
	IndexTupleForm	tform;
	int				natts;
}				IndDesc;

typedef struct
{
	AttributeTupleForm attr;
	Datum			best,
					guess1,
					guess2,
					max,
					min;
	int16			best_len,
					guess1_len,
					guess2_len,
					max_len,
					min_len;
	int32			best_cnt,
					guess1_cnt,
					guess1_hits,
					guess2_hits,
					null_cnt,
					nonnull_cnt;
	int32			max_cnt,
					min_cnt;
	func_ptr		f_cmpeq,
					f_cmplt,
					f_cmpgt;
	regproc			outfunc;
	bool			initialized;
}				VacAttrStats;

typedef struct VRelListData
{
	Oid				vrl_relid;
	struct VRelListData *vrl_next;
}				VRelListData;

typedef VRelListData *VRelList;

typedef struct VRelStats
{
	Oid				relid;
	int				ntups;
	int				npages;
	Size			min_tlen;
	Size			max_tlen;
	bool			hasindex;
	int				va_natts;	/* number of attrs being analyzed */
	VacAttrStats   *vacattrstats;
}				VRelStats;

extern bool		VacuumRunning;

extern void		vc_abort(void);
extern void		vacuum(char *vacrel, bool verbose, bool analyze, List * va_spec);

#define ATTNVALS_SCALE	1000000000		/* XXX so it can act as a float4 */

#endif							/* VACUUM_H */
