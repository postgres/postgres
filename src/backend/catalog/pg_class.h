/*-------------------------------------------------------------------------
 *
 * pg_class.h--
 *    definition of the system "relation" relation (pg_class)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_class.h,v 1.1.1.1.2.1 1996/08/21 04:23:34 scrappy Exp $
 *
 * NOTES
 *    ``pg_relation'' is being replaced by ``pg_class''.  currently
 *    we are only changing the name in the catalogs but someday the
 *    code will be changed too. -cim 2/26/90
 *    [it finally happens.  -ay 11/5/94]
 *
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RELATION_H
#define PG_RELATION_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"
#include "utils/nabstime.h"

/* ----------------
 *	pg_class definition.  cpp turns this into
 *	typedef struct FormData_pg_class
 *
 *	Note: the #if 0, #endif around the BKI_BEGIN.. END block
 *	      below keeps cpp from seeing what is meant for the
 *	      genbki script: pg_relation is now called pg_class, but
 *	      only in the catalogs -cim 2/26/90
 * ----------------
 */

/* ----------------
 *	This structure is actually variable-length (the last attribute is
 *	a POSTGRES array).  Hence, sizeof(FormData_pg_class) does not
 *	describe the fixed-length or actual size of the structure.
 *	FormData_pg_class.relacl may not be correctly aligned, either,
 *	if aclitem and struct varlena don't align together.  Hence,
 *	you MUST use heap_getattr() to get the relacl field.
 * ----------------
 */
CATALOG(pg_class) BOOTSTRAP {
     NameData 	relname;
     Oid        reltype;          
     Oid 	relowner;
     Oid 	relam;
     int4 	relpages;
     int4 	reltuples;
     int4 	relexpires; /* really used as a abstime, but fudge it for now*/
     int4	relpreserved;/*really used as a reltime, but fudge it for now*/
     bool 	relhasindex;
     bool 	relisshared;
     char 	relkind;
     char 	relarch; /* 'h' = heavy, 'l' = light, 'n' = no archival*/
     int2 	relnatts;
       /* relnatts is the number of user attributes this class has.  There 
          must be exactly this many instances in Class pg_attribute for this 
          class which have attnum > 0 (= user attribute).
          */
     int2	relsmgr;
     int28 	relkey;			/* not used */
     oid8	relkeyop;		/* not used */
     bool	relhasrules;
     aclitem	relacl[1];		/* this is here for the catalog */
} FormData_pg_class;

#define CLASS_TUPLE_SIZE \
     (offsetof(FormData_pg_class,relhasrules) + sizeof(bool))

/* ----------------
 *	Form_pg_class corresponds to a pointer to a tuple with
 *	the format of pg_class relation.
 * ----------------
 */
typedef FormData_pg_class	*Form_pg_class;

/* ----------------
 *	compiler constants for pg_class
 * ----------------
 */

/* ----------------
 *	Natts_pg_class_fixed is used to tell routines that insert new
 *	pg_class tuples (as opposed to replacing old ones) that there's no
 *	relacl field.
 * ----------------
 */
#define Natts_pg_class_fixed		17
#define Natts_pg_class			18
#define Anum_pg_class_relname		1
#define Anum_pg_class_reltype           2
#define Anum_pg_class_relowner		3
#define Anum_pg_class_relam		4
#define Anum_pg_class_relpages		5
#define Anum_pg_class_reltuples		6
#define Anum_pg_class_relexpires	7
#define Anum_pg_class_relpreserved	8
#define Anum_pg_class_relhasindex	9
#define Anum_pg_class_relisshared	10
#define Anum_pg_class_relkind		11
#define Anum_pg_class_relarch		12
#define Anum_pg_class_relnatts		13
#define Anum_pg_class_relsmgr		14
#define Anum_pg_class_relkey		15
#define Anum_pg_class_relkeyop		16
#define Anum_pg_class_relhasrules	17
#define Anum_pg_class_relacl		18

/* ----------------
 *	initial contents of pg_class
 * ----------------
 */

DATA(insert OID =  71 (  pg_type 71          PGUID 0 0 0 0 0 f f r n 16 0 - - f _null_ ));
DATA(insert OID =  75 (  pg_attribute 75      PGUID 0 0 0 0 0 f f r n 16 0 - - f _null_ ));
DATA(insert OID =  76 (  pg_demon 76          PGUID 0 0 0 0 0 f t r n 4 0 - - f _null_ ));
DATA(insert OID =  80 (  pg_magic 80         PGUID 0 0 0 0 0 f t r n 2 0 - - f _null_ ));
DATA(insert OID =  81 (  pg_proc 81          PGUID 0 0 0 0 0 f f r n 16 0 - - f _null_ ));
DATA(insert OID =  82 (  pg_server 82         PGUID 0 0 0 0 0 f t r n 3 0 - - f _null_ ));
DATA(insert OID =  83 (  pg_class 83         PGUID 0 0 0 0 0 f f r n 18 0 - - f _null_ ));    
DATA(insert OID =  86 (  pg_user 86          PGUID 0 0 0 0 0 f t r n 6 0 - - f _null_ ));
DATA(insert OID =  87 (  pg_group 87          PGUID 0 0 0 0 0 f t s n 3 0 - - f _null_ ));
DATA(insert OID =  88 (  pg_database 88      PGUID 0 0 0 0 0 f t r n 3 0 - - f _null_ ));
DATA(insert OID =  89 (  pg_defaults 89       PGUID 0 0 0 0 0 f t r n 2 0 - - f _null_ ));
DATA(insert OID =  90 (  pg_variable 90        PGUID 0 0 0 0 0 f t s n 2 0 - - f _null_ ));
DATA(insert OID =  99 (  pg_log  99           PGUID 0 0 0 0 0 f t s n 1 0 - - f _null_ ));
DATA(insert OID = 100 (  pg_time 100           PGUID 0 0 0 0 0 f t s n 1 0 - - f _null_ ));
DATA(insert OID = 101 (  pg_hosts 101           PGUID 0 0 0 0 0 f t s n 3 0 - - f _null_ ));

#define RelOid_pg_type		71
#define RelOid_pg_demon       	76   
#define RelOid_pg_attribute  	75   
#define RelOid_pg_magic   	80      
#define RelOid_pg_proc       	81   
#define RelOid_pg_server     	82   
#define RelOid_pg_class   	83   
#define RelOid_pg_user       	86   
#define RelOid_pg_group       	87
#define RelOid_pg_database    	88   
#define RelOid_pg_defaults  	89    
#define RelOid_pg_variable   	90   
#define RelOid_pg_log   	99       
#define RelOid_pg_time   	100      
#define RelOid_pg_hosts   	101      
    
#define MAX_SYSTEM_RELOID       101

#define       RELKIND_INDEX           'i'     /* secondary index */
#define       RELKIND_RELATION        'r'     /* cataloged heap */
#define       RELKIND_SPECIAL         's'     /* special (non-heap) */
#define       RELKIND_UNCATALOGED     'u'     /* temporary heap */

#endif /* PG_RELATION_H */
