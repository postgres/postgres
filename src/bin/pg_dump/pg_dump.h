/*-------------------------------------------------------------------------
 *
 * pg_dump.h
 *	  Common header file for the pg_dump utility
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_dump.h,v 1.104 2003/08/08 04:52:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DUMP_H
#define PG_DUMP_H

#include "pg_backup.h"

/*
 * The data structures used to store system catalog information
 *
 * NOTE: the structures described here live for the entire pg_dump run;
 * and in most cases we make a struct for every object we can find in the
 * catalogs, not only those we are actually going to dump.	Hence, it's
 * best to store a minimal amount of per-object info in these structs,
 * and retrieve additional per-object info when and if we dump a specific
 * object.	In particular, try to avoid retrieving expensive-to-compute
 * information until it's known to be needed.
 */

typedef struct _namespaceInfo
{
	char	   *oid;
	char	   *nspname;
	char	   *usename;		/* name of owner, or empty string */
	char	   *nspacl;
	bool		dump;			/* true if need to dump definition */
} NamespaceInfo;

typedef struct _typeInfo
{
	char	   *oid;
	char	   *typname;		/* name as seen in catalog */
	/* Note: format_type might produce something different than typname */
	NamespaceInfo *typnamespace;	/* link to containing namespace */
	char	   *usename;		/* name of owner, or empty string */
	char	   *typelem;		/* OID */
	char	   *typrelid;		/* OID */
	char		typrelkind;		/* 'r', 'v', 'c', etc */
	char		typtype;		/* 'b', 'c', etc */
	bool		isArray;		/* true if user-defined array type */
	bool		isDefined;		/* true if typisdefined */
} TypeInfo;

typedef struct _funcInfo
{
	char	   *oid;
	char	   *proname;
	NamespaceInfo *pronamespace;	/* link to containing namespace */
	char	   *usename;		/* name of owner, or empty string */
	Oid			lang;
	int			nargs;
	char	  **argtypes;		/* OIDs */
	char	   *prorettype;		/* OID */
	char	   *proacl;
	bool		dumped;			/* true if already dumped */
} FuncInfo;

typedef struct _aggInfo
{
	char	   *oid;
	char	   *aggname;
	char	   *aggbasetype;	/* OID */
	NamespaceInfo *aggnamespace;	/* link to containing namespace */
	char	   *usename;
	char	   *aggacl;
	bool		anybasetype;	/* is the basetype "any"? */
	char	   *fmtbasetype;	/* formatted type name */
} AggInfo;

typedef struct _oprInfo
{
	char	   *oid;
	char	   *oprname;
	NamespaceInfo *oprnamespace;	/* link to containing namespace */
	char	   *usename;
	char	   *oprcode;		/* as OID, not regproc name */
} OprInfo;

typedef struct _opclassInfo
{
	char	   *oid;
	char	   *opcname;
	NamespaceInfo *opcnamespace;	/* link to containing namespace */
	char	   *usename;
} OpclassInfo;

typedef struct _tableInfo
{
	/*
	 * These fields are collected for every table in the database.
	 */
	char	   *oid;
	char	   *relname;
	NamespaceInfo *relnamespace;	/* link to containing namespace */
	char	   *usename;		/* name of owner, or empty string */
	char	   *relacl;
	char		relkind;
	bool		hasindex;		/* does it have any indexes? */
	bool		hasrules;		/* does it have any rules? */
	bool		hasoids;		/* does it have OIDs? */
	int			ncheck;			/* # of CHECK expressions */
	int			ntrig;			/* # of triggers */
	/* these two are set only if table is a SERIAL column's sequence: */
	char	   *owning_tab;		/* OID of table owning sequence */
	int			owning_col;		/* attr # of column owning sequence */

	bool		interesting;	/* true if need to collect more data */
	bool		dump;			/* true if we want to dump it */

	/*
	 * These fields are computed only if we decide the table is
	 * interesting (it's either a table to dump, or a direct parent of a
	 * dumpable table).
	 */
	int			numatts;		/* number of attributes */
	char	  **attnames;		/* the attribute names */
	char	  **atttypnames;	/* attribute type names */
	int		   *atttypmod;		/* type-specific type modifiers */
	int		   *attstattarget;	/* attribute statistics targets */
	char	   *attstorage;		/* attribute storage scheme */
	char	   *typstorage;		/* type storage scheme */
	bool	   *attisdropped;	/* true if attr is dropped; don't dump it */
	bool	   *attislocal;		/* true if attr has local definition */
	bool	   *attisserial;	/* true if attr is serial or bigserial */

	/*
	 * Note: we need to store per-attribute notnull and default stuff for
	 * all interesting tables so that we can tell which constraints were
	 * inherited.
	 */
	bool	   *notnull;		/* Not null constraints on attributes */
	char	  **adef_expr;		/* DEFAULT expressions */
	bool	   *inhAttrs;		/* true if each attribute is inherited */
	bool	   *inhAttrDef;		/* true if attr's default is inherited */
	bool	   *inhNotNull;		/* true if NOT NULL is inherited */

	/*
	 * Stuff computed only for dumpable tables.
	 */
	int			numParents;		/* number of (immediate) parent tables */
	int		   *parentIndexes;	/* TableInfo indexes of immediate parents */

	char	   *viewoid;		/* OID of view - should be >= oid of table
								 * important because views may be
								 * constructed manually from rules, and
								 * rule may ref things created after the
								 * base table was created. */
} TableInfo;

typedef struct _inhInfo
{
	char	   *inhrelid;		/* OID of a child table */
	char	   *inhparent;		/* OID of its parent */
} InhInfo;


/* global decls */
extern bool force_quotes;		/* double-quotes for identifiers flag */
extern bool g_verbose;			/* verbose flag */
extern Archive *g_fout;			/* the script file */

/* placeholders for comment starting and ending delimiters */
extern char g_comment_start[10];
extern char g_comment_end[10];

extern char g_opaque_type[10];	/* name for the opaque type */

/*
 *	common utility functions
 */

extern TableInfo *dumpSchema(Archive *fout,
		   int *numTablesPtr,
		   const bool aclsSkip,
		   const bool schemaOnly,
		   const bool dataOnly);

typedef enum _OidOptions
{
	zeroAsOpaque = 1,
	zeroAsAny = 2,
	zeroAsStar = 4,
	zeroAsNone = 8
} OidOptions;

extern int	findTableByOid(TableInfo *tbinfo, int numTables, const char *oid);
extern char *findOprByOid(OprInfo *oprinfo, int numOprs, const char *oid);
extern int	findFuncByOid(FuncInfo *finfo, int numFuncs, const char *oid);
extern int	findTypeByOid(TypeInfo *tinfo, int numTypes, const char *oid);

extern void check_conn_and_db(void);
extern void exit_nicely(void);

extern void parseNumericArray(const char *str, char **array, int arraysize);

/*
 * version specific routines
 */
extern NamespaceInfo *getNamespaces(int *numNamespaces);
extern TypeInfo *getTypes(int *numTypes);
extern FuncInfo *getFuncs(int *numFuncs);
extern AggInfo *getAggregates(int *numAggregates);
extern OprInfo *getOperators(int *numOperators);
extern OpclassInfo *getOpclasses(int *numOpclasses);
extern TableInfo *getTables(int *numTables);
extern InhInfo *getInherits(int *numInherits);

extern void getTableAttrs(TableInfo *tbinfo, int numTables);
extern void dumpDBComment(Archive *outfile);
extern void dumpNamespaces(Archive *fout,
			   NamespaceInfo *nsinfo, int numNamespaces);
extern void dumpTypes(Archive *fout, FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes);
extern void dumpProcLangs(Archive *fout, FuncInfo finfo[], int numFuncs);
extern void dumpFuncs(Archive *fout, FuncInfo finfo[], int numFuncs);
extern void dumpCasts(Archive *fout, FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes);
extern void dumpAggs(Archive *fout, AggInfo agginfo[], int numAggregates);
extern void dumpOprs(Archive *fout, OprInfo *oprinfo, int numOperators);
extern void dumpOpclasses(Archive *fout,
			  OpclassInfo *opcinfo, int numOpclasses);
extern void dumpTables(Archive *fout, TableInfo tblinfo[], int numTables,
		   const bool aclsSkip,
		   const bool schemaOnly, const bool dataOnly);
extern void dumpIndexes(Archive *fout, TableInfo *tbinfo, int numTables);

#endif   /* PG_DUMP_H */
