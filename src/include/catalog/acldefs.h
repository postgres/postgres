/*-------------------------------------------------------------------------
 *
 * acldefs.h
 *	  base definitions for ACLs and role attributes
 *
 * Portions Copyright (c) 2014, PostgreSQL Global Development Group
 *
 * src/include/catalog/acldefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACLDEFS_H
#define ACLDEFS_H

/*
 * Grantable rights are encoded so that we can OR them together in a bitmask.
 * The present representation of AclItem limits us to 16 distinct rights,
 * even though AclMode is defined as uint32.  See utils/acl.h.
 *
 * Caution: changing these codes breaks stored ACLs, hence forces initdb.
 */
typedef uint32 AclMode;			/* a bitmask of privilege bits */

#define ACL_INSERT		(1<<0)	/* for relations */
#define ACL_SELECT		(1<<1)
#define ACL_UPDATE		(1<<2)
#define ACL_DELETE		(1<<3)
#define ACL_TRUNCATE	(1<<4)
#define ACL_REFERENCES	(1<<5)
#define ACL_TRIGGER		(1<<6)
#define ACL_EXECUTE		(1<<7)	/* for functions */
#define ACL_USAGE		(1<<8)	/* for languages, namespaces, FDWs, and
								 * servers */
#define ACL_CREATE		(1<<9)	/* for namespaces and databases */
#define ACL_CREATE_TEMP (1<<10) /* for databases */
#define ACL_CONNECT		(1<<11) /* for databases */
#define N_ACL_RIGHTS	12		/* 1 plus the last 1<<x */
#define ACL_NO_RIGHTS	0
/* Currently, SELECT ... FOR [KEY] UPDATE/SHARE requires UPDATE privileges */
#define ACL_SELECT_FOR_UPDATE	ACL_UPDATE

#define ACL_ID_PUBLIC	0		/* placeholder for id in a PUBLIC acl item */


/*
 * Role attributes are encoded so that we can OR them together in a bitmask.
 * The present representation of RoleAttr (defined in acl.h) limits us to 64
 * distinct rights.
 *
 * Note about ROLE_ATTR_ALL: This symbol is used verbatim by genbki.pl, which
 * means we need to hard-code its value instead of using a symbolic definition.
 * Therefore, whenever role attributes are changed, this value MUST be updated
 * manually.
 */

/* A bitmask for role attributes */
typedef uint64 RoleAttr;

#define ROLE_ATTR_NONE			0
#define ROLE_ATTR_SUPERUSER		(1<<0)
#define ROLE_ATTR_INHERIT		(1<<1)
#define ROLE_ATTR_CREATEROLE	(1<<2)
#define ROLE_ATTR_CREATEDB		(1<<3)
#define ROLE_ATTR_CATUPDATE		(1<<4)
#define ROLE_ATTR_CANLOGIN		(1<<5)
#define ROLE_ATTR_REPLICATION	(1<<6)
#define ROLE_ATTR_BYPASSRLS		(1<<7)
#define N_ROLE_ATTRIBUTES		8		/* 1 plus the last 1<<x */
#define ROLE_ATTR_ALL			255		/* (1 << N_ROLE_ATTRIBUTES) - 1 */


#endif   /* ACLDEFS_H */
