/*-------------------------------------------------------------------------
 *
 * pg_group.h
 *
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_group.h,v 1.17 2003/08/04 02:40:12 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_GROUP_H
#define PG_GROUP_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

CATALOG(pg_group) BOOTSTRAP BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	NameData	groname;
	int4		grosysid;
	int4		grolist[1];
} FormData_pg_group;

/* VARIABLE LENGTH STRUCTURE */

typedef FormData_pg_group *Form_pg_group;

#define Natts_pg_group			3
#define Anum_pg_group_groname	1
#define Anum_pg_group_grosysid	2
#define Anum_pg_group_grolist	3

#endif   /* PG_GROUP_H */
