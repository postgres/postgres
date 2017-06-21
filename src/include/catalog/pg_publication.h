/*-------------------------------------------------------------------------
 *
 * pg_publication.h
 *	  definition of the relation sets relation (pg_publication)
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_publication.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PUBLICATION_H
#define PG_PUBLICATION_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"

/* ----------------
 *		pg_publication definition.  cpp turns this into
 *		typedef struct FormData_pg_publication
 *
 * ----------------
 */
#define PublicationRelationId			6104

CATALOG(pg_publication,6104)
{
	NameData	pubname;		/* name of the publication */

	Oid			pubowner;		/* publication owner */

	/*
	 * indicates that this is special publication which should encompass all
	 * tables in the database (except for the unlogged and temp ones)
	 */
	bool		puballtables;

	/* true if inserts are published */
	bool		pubinsert;

	/* true if updates are published */
	bool		pubupdate;

	/* true if deletes are published */
	bool		pubdelete;

} FormData_pg_publication;

/* ----------------
 *		Form_pg_publication corresponds to a pointer to a tuple with
 *		the format of pg_publication relation.
 * ----------------
 */
typedef FormData_pg_publication *Form_pg_publication;

/* ----------------
 *		compiler constants for pg_publication
 * ----------------
 */

#define Natts_pg_publication				6
#define Anum_pg_publication_pubname			1
#define Anum_pg_publication_pubowner		2
#define Anum_pg_publication_puballtables	3
#define Anum_pg_publication_pubinsert		4
#define Anum_pg_publication_pubupdate		5
#define Anum_pg_publication_pubdelete		6

typedef struct PublicationActions
{
	bool		pubinsert;
	bool		pubupdate;
	bool		pubdelete;
} PublicationActions;

typedef struct Publication
{
	Oid			oid;
	char	   *name;
	bool		alltables;
	PublicationActions pubactions;
} Publication;

extern Publication *GetPublication(Oid pubid);
extern Publication *GetPublicationByName(const char *pubname, bool missing_ok);
extern List *GetRelationPublications(Oid relid);
extern List *GetPublicationRelations(Oid pubid);
extern List *GetAllTablesPublications(void);
extern List *GetAllTablesPublicationRelations(void);

extern ObjectAddress publication_add_relation(Oid pubid, Relation targetrel,
						 bool if_not_exists);

extern Oid	get_publication_oid(const char *pubname, bool missing_ok);
extern char *get_publication_name(Oid pubid);

extern Datum pg_get_publication_tables(PG_FUNCTION_ARGS);

#endif							/* PG_PUBLICATION_H */
