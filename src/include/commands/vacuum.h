/*-------------------------------------------------------------------------
 *
 * vacuum.h--
 *    header file for postgres vacuum cleaner
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.2 1996/10/18 08:15:58 vadim Exp $
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
    BlockNumber			vpd_blkno;	/* BlockNumber of this Page */
    Size			vpd_free;	/* FreeSpace on this Page */
    uint16			vpd_noff;	/* Number of dead tids */
    OffsetNumber		vpd_voff[1];	/* Array of its OffNums */
} VPageDescrData;

typedef VPageDescrData	*VPageDescr;

typedef struct VRelListData {
    Oid			vrl_relid;
    VAttList		vrl_attlist;
    VPageDescr		*vrl_pgdsc;
    int			vrl_nrepg;
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
