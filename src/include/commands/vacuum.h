/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.30 2000/05/29 17:06:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include "fmgr.h"
#include "access/funcindex.h"
#include "catalog/pg_index.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"
#include "storage/itemptr.h"


typedef struct VAttListData
{
	int			val_dummy;
	struct VAttListData *val_next;
} VAttListData;

typedef VAttListData *VAttList;

typedef struct VacPageData
{
	BlockNumber blkno;		/* BlockNumber of this Page */
	Size		free;		/* FreeSpace on this Page */
	uint16		offsets_used;		/* Number of OffNums used by
										 * vacuum */
	uint16		offsets_free;		/* Number of OffNums free or to be
										 * free */
	OffsetNumber offsets[1];/* Array of its OffNums */
} VacPageData;

typedef VacPageData *VacPage;

typedef struct VacPageListData
{
	int			empty_end_pages;	/* Number of "empty" end-pages */
	int			num_pages;	/* Number of pages in pagedesc */
	int			num_allocated_pages;		/* Number of allocated
												 * pages in pagedesc */
	VacPage *pagedesc;	/* Descriptions of pages */
} VacPageListData;

typedef VacPageListData *VacPageList;

typedef struct
{
	FuncIndexInfo finfo;
	FuncIndexInfo *finfoP;
	Form_pg_index tform;
	int			natts;
} IndDesc;

typedef struct
{
	Form_pg_attribute attr;
	Datum		best,
				guess1,
				guess2,
				max,
				min;
	int			best_len,
				guess1_len,
				guess2_len,
				max_len,
				min_len;
	long		best_cnt,
				guess1_cnt,
				guess1_hits,
				guess2_hits,
				null_cnt,
				nonnull_cnt,
				max_cnt,
				min_cnt;
	FmgrInfo	f_cmpeq,
				f_cmplt,
				f_cmpgt;
	Oid			op_cmplt;
	regproc		outfunc;
	Oid			typelem;
	bool		initialized;
} VacAttrStats;

typedef struct VRelListData
{
	Oid			vrl_relid;
	struct VRelListData *vrl_next;
} VRelListData;

typedef VRelListData *VRelList;

typedef struct VTupleLinkData
{
	ItemPointerData new_tid;
	ItemPointerData this_tid;
} VTupleLinkData;

typedef VTupleLinkData *VTupleLink;

typedef struct VTupleMoveData
{
	ItemPointerData tid;		/* tuple ID */
	VacPage		vacpage;			/* where to move */
	bool		cleanVpd;		/* clean vacpage before using */
} VTupleMoveData;

typedef VTupleMoveData *VTupleMove;

typedef struct VRelStats
{
	Oid			relid;
	int			num_tuples;
	int			num_pages;
	Size		min_tlen;
	Size		max_tlen;
	bool		hasindex;
	int			num_vtlinks;
	VTupleLink	vtlinks;
} VRelStats;

extern bool VacuumRunning;

extern void vc_abort(void);
extern void vacuum(char *vacrel, bool verbose, bool analyze, List *anal_cols);

#define ATTNVALS_SCALE	1000000000		/* XXX so it can act as a float4 */

#endif	 /* VACUUM_H */
