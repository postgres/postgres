#ifndef RELFILENODE_H
#define RELFILENODE_H

/*
 * This is temporal place holder for Relation File Node till
 * reloid.version/unique_id file naming is not implemented
 */
typedef struct RelFileNode
{
	Oid					dbId;		/* database */
	Oid					relId;		/* relation */
} RelFileNode;

#endif	/* RELFILENODE_H */
