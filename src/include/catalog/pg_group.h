/*-------------------------------------------------------------------------
 *
 * pg_group.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_group.h,v 1.1 1996/08/28 01:56:45 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_GROUP_H
#define PG_GROUP_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

CATALOG(pg_group) BOOTSTRAP {
        NameData groname;
        int4    grosysid;
	int4	grolist[1];
} FormData_pg_group;
/* VARIABLE LENGTH STRUCTURE */

typedef FormData_pg_group	*Form_pg_group;

#define Natts_pg_group		1
#define Anum_pg_group_groname	1
#define Anum_pg_group_grosysid	2
#define Anum_pg_group_grolist	3

#endif /* PG_GROUP_H */
