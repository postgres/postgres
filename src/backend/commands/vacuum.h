/*-------------------------------------------------------------------------
 *
 * vacuum.h--
 *    header file for postgres vacuum cleaner
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: vacuum.h,v 1.1.1.1 1996/07/09 06:21:23 scrappy Exp $
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

typedef struct VTidListData {
    ItemPointerData	vtl_tid;
    struct VTidListData	*vtl_next;
} VTidListData;

typedef VTidListData	*VTidList;

typedef struct VRelListData {
    Oid			vrl_relid;
    VAttList		vrl_attlist;
    VTidList		vrl_tidlist;
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
