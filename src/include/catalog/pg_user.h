/*-------------------------------------------------------------------------
 *
 * pg_user.h--
 *    definition of the system "user" relation (pg_user)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_user.h,v 1.1 1996/08/28 01:57:20 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_USER_H
#define PG_USER_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_user definition.  cpp turns this into
 *	typedef struct FormData_pg_user
 * ----------------
 */ 
CATALOG(pg_user) BOOTSTRAP {
    NameData 	usename;
    int4 	usesysid;
    bool 	usecreatedb;
    bool 	usetrace;
    bool 	usesuper;
    bool 	usecatupd;  
} FormData_pg_user;

/* ----------------
 *	Form_pg_user corresponds to a pointer to a tuple with
 *	the format of pg_user relation.
 * ----------------
 */
typedef FormData_pg_user	*Form_pg_user;

/* ----------------
 *	compiler constants for pg_user
 * ----------------
 */
#define Natts_pg_user			6
#define Anum_pg_user_usename		1
#define Anum_pg_user_usesysid		2
#define Anum_pg_user_usecreatedb	3
#define Anum_pg_user_usetrace		4
#define Anum_pg_user_usesuper		5
#define Anum_pg_user_usecatupd		6

/* ----------------
 *	initial contents of pg_user
 * ----------------
 */
DATA(insert OID = 0 ( postgres PGUID t t t t ));

BKI_BEGIN
#ifdef ALLOW_PG_GROUP
BKI_END

DATA(insert OID = 0 ( mike 799 t t t t ));
DATA(insert OID = 0 ( mao 1806 t t t t ));
DATA(insert OID = 0 ( hellers 1089 t t t t ));
DATA(insert OID = 0 ( joey 5209 t t t t ));
DATA(insert OID = 0 ( jolly 5443 t t t t ));
DATA(insert OID = 0 ( sunita 6559 t t t t ));
DATA(insert OID = 0 ( paxson 3029 t t t t ));
DATA(insert OID = 0 ( marc 2435 t t t t ));
DATA(insert OID = 0 ( jiangwu 6124 t t t t ));
DATA(insert OID = 0 ( aoki 2360 t t t t ));
DATA(insert OID = 0 ( avi 31080 t t t t ));
DATA(insert OID = 0 ( kristin 1123 t t t t ));
DATA(insert OID = 0 ( andrew 5229 t t t t ));
DATA(insert OID = 0 ( nobuko 5493 t t t t ));
DATA(insert OID = 0 ( hartzell 6676 t t t t ));
DATA(insert OID = 0 ( devine 6724 t t t t ));
DATA(insert OID = 0 ( boris 6396 t t t t ));
DATA(insert OID = 0 ( sklower 354 t t t t ));
DATA(insert OID = 0 ( marcel 31113 t t t t ));
DATA(insert OID = 0 ( ginger 3692 t t t t ));
DATA(insert OID = 0 ( woodruff 31026 t t t t ));
DATA(insert OID = 0 ( searcher 8261 t t t t ));
     
BKI_BEGIN
#endif /* ALLOW_PG_GROUP */
BKI_END

#endif /* PG_USER_H */
