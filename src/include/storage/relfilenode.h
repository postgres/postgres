#ifndef RELFILENODE_H
#define RELFILENODE_H

/*
 * This is all what we need to know to find relation file.
 * tblNode is identificator of tablespace and because of
 * currently our tablespaces are equal to databases this is
 * database OID. relNode is currently relation OID on creation
 * but may be changed later if required. relNode is stored in
 * pg_class.relfilenode.
 */
typedef struct RelFileNode
{
	Oid					tblNode;		/* tablespace */
	Oid					relNode;		/* relation */
} RelFileNode;

#endif	/* RELFILENODE_H */
