/*-------------------------------------------------------------------------
 *
 * vacuum.h--
 *    header file for postgres vacuum cleaner
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.3 1996/11/27 07:35:06 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	VACUUM_H
#define	VACUUM_H

typedef struct VAttListData {
    int			val_dummy;
    struct VAttListData	*val_next;
} VAttListData;

typedef VAttListData	*VAttList;

typedef struct VPageDescrData {
    BlockNumber		vpd_blkno;	/* BlockNumber of this Page */
    Size		vpd_free;	/* FreeSpace on this Page */
    uint16		vpd_nusd;	/* Number of OffNums used by vacuum */
    uint16		vpd_noff;	/* Number of OffNums free or to be free */
    OffsetNumber	vpd_voff[1];	/* Array of its OffNums */
} VPageDescrData;

typedef VPageDescrData	*VPageDescr;

typedef struct VPageListData {
    int			vpl_nemend;	/* Number of "empty" end-pages */
    int			vpl_npages;	/* Number of pages in vpl_pgdesc */
    VPageDescr		*vpl_pgdesc;	/* Descriptions of pages */
} VPageListData;

typedef VPageListData	*VPageList;

typedef struct VRelListData {
    Oid			vrl_relid;
    VAttList		vrl_attlist;
    int			vrl_ntups;
    int			vrl_npages;
    bool		vrl_hasindex;
    struct VRelListData	*vrl_next;
} VRelListData;

typedef VRelListData	*VRelList;

extern bool VacuumRunning;

extern void vc_abort(void);
extern void vacuum(char *vacrel);


#endif	/* VACUUM_H */
