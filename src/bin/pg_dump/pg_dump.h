/*-------------------------------------------------------------------------
 *
 * pg_dump.h
 *	  Common header file for the pg_dump utility
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_dump.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DUMP_H
#define PG_DUMP_H

#include "pg_backup.h"


#define oidcmp(x,y) ( ((x) < (y) ? -1 : ((x) > (y)) ?  1 : 0) )

/*
 * The data structures used to store system catalog information.  Every
 * dumpable object is a subclass of DumpableObject.
 *
 * NOTE: the structures described here live for the entire pg_dump run;
 * and in most cases we make a struct for every object we can find in the
 * catalogs, not only those we are actually going to dump.  Hence, it's
 * best to store a minimal amount of per-object info in these structs,
 * and retrieve additional per-object info when and if we dump a specific
 * object.  In particular, try to avoid retrieving expensive-to-compute
 * information until it's known to be needed.  We do, however, have to
 * store enough info to determine whether an object should be dumped and
 * what order to dump in.
 */

typedef enum
{
	/* When modifying this enum, update priority tables in pg_dump_sort.c! */
	DO_NAMESPACE,
	DO_EXTENSION,
	DO_TYPE,
	DO_SHELL_TYPE,
	DO_FUNC,
	DO_AGG,
	DO_OPERATOR,
	DO_ACCESS_METHOD,
	DO_OPCLASS,
	DO_OPFAMILY,
	DO_COLLATION,
	DO_CONVERSION,
	DO_TABLE,
	DO_ATTRDEF,
	DO_INDEX,
	DO_INDEX_ATTACH,
	DO_STATSEXT,
	DO_RULE,
	DO_TRIGGER,
	DO_CONSTRAINT,
	DO_FK_CONSTRAINT,			/* see note for ConstraintInfo */
	DO_PROCLANG,
	DO_CAST,
	DO_TABLE_DATA,
	DO_SEQUENCE_SET,
	DO_DUMMY_TYPE,
	DO_TSPARSER,
	DO_TSDICT,
	DO_TSTEMPLATE,
	DO_TSCONFIG,
	DO_FDW,
	DO_FOREIGN_SERVER,
	DO_DEFAULT_ACL,
	DO_TRANSFORM,
	DO_BLOB,
	DO_BLOB_DATA,
	DO_PRE_DATA_BOUNDARY,
	DO_POST_DATA_BOUNDARY,
	DO_EVENT_TRIGGER,
	DO_REFRESH_MATVIEW,
	DO_POLICY,
	DO_PUBLICATION,
	DO_PUBLICATION_REL,
	DO_SUBSCRIPTION
} DumpableObjectType;

/* component types of an object which can be selected for dumping */
typedef uint32 DumpComponents;	/* a bitmask of dump object components */
#define DUMP_COMPONENT_NONE			(0)
#define DUMP_COMPONENT_DEFINITION	(1 << 0)
#define DUMP_COMPONENT_DATA			(1 << 1)
#define DUMP_COMPONENT_COMMENT		(1 << 2)
#define DUMP_COMPONENT_SECLABEL		(1 << 3)
#define DUMP_COMPONENT_ACL			(1 << 4)
#define DUMP_COMPONENT_POLICY		(1 << 5)
#define DUMP_COMPONENT_USERMAP		(1 << 6)
#define DUMP_COMPONENT_ALL			(0xFFFF)

/*
 * component types which require us to obtain a lock on the table
 *
 * Note that some components only require looking at the information
 * in the pg_catalog tables and, for those components, we do not need
 * to lock the table.  Be careful here though- some components use
 * server-side functions which pull the latest information from
 * SysCache and in those cases we *do* need to lock the table.
 *
 * We do not need locks for the COMMENT and SECLABEL components as
 * those simply query their associated tables without using any
 * server-side functions.  We do not need locks for the ACL component
 * as we pull that information from pg_class without using any
 * server-side functions that use SysCache.  The USERMAP component
 * is only relevant for FOREIGN SERVERs and not tables, so no sense
 * locking a table for that either (that can happen if we are going
 * to dump "ALL" components for a table).
 *
 * We DO need locks for DEFINITION, due to various server-side
 * functions that are used and POLICY due to pg_get_expr().  We set
 * this up to grab the lock except in the cases we know to be safe.
 */
#define DUMP_COMPONENTS_REQUIRING_LOCK (\
		DUMP_COMPONENT_DEFINITION |\
		DUMP_COMPONENT_DATA |\
		DUMP_COMPONENT_POLICY)

typedef struct _dumpableObject
{
	DumpableObjectType objType;
	CatalogId	catId;			/* zero if not a cataloged object */
	DumpId		dumpId;			/* assigned by AssignDumpId() */
	char	   *name;			/* object name (should never be NULL) */
	struct _namespaceInfo *namespace;	/* containing namespace, or NULL */
	DumpComponents dump;		/* bitmask of components to dump */
	DumpComponents dump_contains;	/* as above, but for contained objects */
	bool		ext_member;		/* true if object is member of extension */
	bool		depends_on_ext; /* true if object depends on an extension */
	DumpId	   *dependencies;	/* dumpIds of objects this one depends on */
	int			nDeps;			/* number of valid dependencies */
	int			allocDeps;		/* allocated size of dependencies[] */
} DumpableObject;

typedef struct _namespaceInfo
{
	DumpableObject dobj;
	char	   *rolname;		/* name of owner, or empty string */
	char	   *nspacl;
	char	   *rnspacl;
	char	   *initnspacl;
	char	   *initrnspacl;
} NamespaceInfo;

typedef struct _extensionInfo
{
	DumpableObject dobj;
	char	   *namespace;		/* schema containing extension's objects */
	bool		relocatable;
	char	   *extversion;
	char	   *extconfig;		/* info about configuration tables */
	char	   *extcondition;
} ExtensionInfo;

typedef struct _typeInfo
{
	DumpableObject dobj;

	/*
	 * Note: dobj.name is the raw pg_type.typname entry.  ftypname is the
	 * result of format_type(), which will be quoted if needed, and might be
	 * schema-qualified too.
	 */
	char	   *ftypname;
	char	   *rolname;		/* name of owner, or empty string */
	char	   *typacl;
	char	   *rtypacl;
	char	   *inittypacl;
	char	   *initrtypacl;
	Oid			typelem;
	Oid			typrelid;
	char		typrelkind;		/* 'r', 'v', 'c', etc */
	char		typtype;		/* 'b', 'c', etc */
	bool		isArray;		/* true if auto-generated array type */
	bool		isDefined;		/* true if typisdefined */
	/* If needed, we'll create a "shell type" entry for it; link that here: */
	struct _shellTypeInfo *shellType;	/* shell-type entry, or NULL */
	/* If it's a domain, we store links to its constraints here: */
	int			nDomChecks;
	struct _constraintInfo *domChecks;
} TypeInfo;

typedef struct _shellTypeInfo
{
	DumpableObject dobj;

	TypeInfo   *baseType;		/* back link to associated base type */
} ShellTypeInfo;

typedef struct _funcInfo
{
	DumpableObject dobj;
	char	   *rolname;		/* name of owner, or empty string */
	Oid			lang;
	int			nargs;
	Oid		   *argtypes;
	Oid			prorettype;
	char	   *proacl;
	char	   *rproacl;
	char	   *initproacl;
	char	   *initrproacl;
} FuncInfo;

/* AggInfo is a superset of FuncInfo */
typedef struct _aggInfo
{
	FuncInfo	aggfn;
	/* we don't require any other fields at the moment */
} AggInfo;

typedef struct _oprInfo
{
	DumpableObject dobj;
	char	   *rolname;
	char		oprkind;
	Oid			oprcode;
} OprInfo;

typedef struct _accessMethodInfo
{
	DumpableObject dobj;
	char		amtype;
	char	   *amhandler;
} AccessMethodInfo;

typedef struct _opclassInfo
{
	DumpableObject dobj;
	char	   *rolname;
} OpclassInfo;

typedef struct _opfamilyInfo
{
	DumpableObject dobj;
	char	   *rolname;
} OpfamilyInfo;

typedef struct _collInfo
{
	DumpableObject dobj;
	char	   *rolname;
} CollInfo;

typedef struct _convInfo
{
	DumpableObject dobj;
	char	   *rolname;
} ConvInfo;

typedef struct _tableInfo
{
	/*
	 * These fields are collected for every table in the database.
	 */
	DumpableObject dobj;
	char	   *rolname;		/* name of owner, or empty string */
	char	   *relacl;
	char	   *rrelacl;
	char	   *initrelacl;
	char	   *initrrelacl;
	char		relkind;
	char		relpersistence; /* relation persistence */
	bool		relispopulated; /* relation is populated */
	char		relreplident;	/* replica identifier */
	char	   *reltablespace;	/* relation tablespace */
	char	   *reloptions;		/* options specified by WITH (...) */
	char	   *checkoption;	/* WITH CHECK OPTION, if any */
	char	   *toast_reloptions;	/* WITH options for the TOAST table */
	bool		hasindex;		/* does it have any indexes? */
	bool		hasrules;		/* does it have any rules? */
	bool		hastriggers;	/* does it have any triggers? */
	bool		rowsec;			/* is row security enabled? */
	bool		forcerowsec;	/* is row security forced? */
	bool		hasoids;		/* does it have OIDs? */
	uint32		frozenxid;		/* table's relfrozenxid */
	uint32		minmxid;		/* table's relminmxid */
	Oid			toast_oid;		/* toast table's OID, or 0 if none */
	uint32		toast_frozenxid;	/* toast table's relfrozenxid, if any */
	uint32		toast_minmxid;	/* toast table's relminmxid */
	int			ncheck;			/* # of CHECK expressions */
	Oid			reloftype;		/* underlying type for typed table */
	Oid			foreign_server; /* foreign server oid, if applicable */
	/* these two are set only if table is a sequence owned by a column: */
	Oid			owning_tab;		/* OID of table owning sequence */
	int			owning_col;		/* attr # of column owning sequence */
	bool		is_identity_sequence;
	int			relpages;		/* table's size in pages (from pg_class) */

	bool		interesting;	/* true if need to collect more data */
	bool		dummy_view;		/* view's real definition must be postponed */
	bool		postponed_def;	/* matview must be postponed into post-data */
	bool		ispartition;	/* is table a partition? */
	bool		unsafe_partitions;	/* is it an unsafe partitioned table? */

	/*
	 * These fields are computed only if we decide the table is interesting
	 * (it's either a table to dump, or a direct parent of a dumpable table).
	 */
	int			numatts;		/* number of attributes */
	char	  **attnames;		/* the attribute names */
	char	  **atttypnames;	/* attribute type names */
	int		   *atttypmod;		/* type-specific type modifiers */
	int		   *attstattarget;	/* attribute statistics targets */
	char	   *attstorage;		/* attribute storage scheme */
	char	   *typstorage;		/* type storage scheme */
	bool	   *attisdropped;	/* true if attr is dropped; don't dump it */
	char	   *attidentity;
	char	   *attgenerated;
	int		   *attlen;			/* attribute length, used by binary_upgrade */
	char	   *attalign;		/* attribute align, used by binary_upgrade */
	bool	   *attislocal;		/* true if attr has local definition */
	char	  **attoptions;		/* per-attribute options */
	Oid		   *attcollation;	/* per-attribute collation selection */
	char	  **attfdwoptions;	/* per-attribute fdw options */
	char	  **attmissingval;	/* per attribute missing value */
	bool	   *notnull;		/* NOT NULL constraints on attributes */
	bool	   *inhNotNull;		/* true if NOT NULL is inherited */
	struct _attrDefInfo **attrdefs; /* DEFAULT expressions */
	struct _constraintInfo *checkexprs; /* CHECK constraints */
	bool		needs_override; /* has GENERATED ALWAYS AS IDENTITY */
	char	   *amname;			/* relation access method */

	/*
	 * Stuff computed only for dumpable tables.
	 */
	int			numParents;		/* number of (immediate) parent tables */
	struct _tableInfo **parents;	/* TableInfos of immediate parents */
	int			numIndexes;		/* number of indexes */
	struct _indxInfo *indexes;	/* indexes */
	struct _tableDataInfo *dataObj; /* TableDataInfo, if dumping its data */
	int			numTriggers;	/* number of triggers for table */
	struct _triggerInfo *triggers;	/* array of TriggerInfo structs */
} TableInfo;

typedef struct _attrDefInfo
{
	DumpableObject dobj;		/* note: dobj.name is name of table */
	TableInfo  *adtable;		/* link to table of attribute */
	int			adnum;
	char	   *adef_expr;		/* decompiled DEFAULT expression */
	bool		separate;		/* true if must dump as separate item */
} AttrDefInfo;

typedef struct _tableDataInfo
{
	DumpableObject dobj;
	TableInfo  *tdtable;		/* link to table to dump */
	char	   *filtercond;		/* WHERE condition to limit rows dumped */
} TableDataInfo;

typedef struct _indxInfo
{
	DumpableObject dobj;
	TableInfo  *indextable;		/* link to table the index is for */
	char	   *indexdef;
	char	   *tablespace;		/* tablespace in which index is stored */
	char	   *indreloptions;	/* options specified by WITH (...) */
	char	   *indstatcols;	/* column numbers with statistics */
	char	   *indstatvals;	/* statistic values for columns */
	int			indnkeyattrs;	/* number of index key attributes */
	int			indnattrs;		/* total number of index attributes */
	Oid		   *indkeys;		/* In spite of the name 'indkeys' this field
								 * contains both key and nonkey attributes */
	bool		indisclustered;
	bool		indisreplident;
	Oid			parentidx;		/* if a partition, parent index OID */
	SimplePtrList partattaches; /* if partitioned, partition attach objects */

	/* if there is an associated constraint object, its dumpId: */
	DumpId		indexconstraint;
} IndxInfo;

typedef struct _indexAttachInfo
{
	DumpableObject dobj;
	IndxInfo   *parentIdx;		/* link to index on partitioned table */
	IndxInfo   *partitionIdx;	/* link to index on partition */
} IndexAttachInfo;

typedef struct _statsExtInfo
{
	DumpableObject dobj;
	char	   *rolname;		/* name of owner, or empty string */
	int			stattarget;		/* statistics target */
} StatsExtInfo;

typedef struct _ruleInfo
{
	DumpableObject dobj;
	TableInfo  *ruletable;		/* link to table the rule is for */
	char		ev_type;
	bool		is_instead;
	char		ev_enabled;
	bool		separate;		/* true if must dump as separate item */
	/* separate is always true for non-ON SELECT rules */
} RuleInfo;

typedef struct _triggerInfo
{
	DumpableObject dobj;
	TableInfo  *tgtable;		/* link to table the trigger is for */
	char	   *tgfname;
	int			tgtype;
	int			tgnargs;
	char	   *tgargs;
	bool		tgisconstraint;
	char	   *tgconstrname;
	Oid			tgconstrrelid;
	char	   *tgconstrrelname;
	char		tgenabled;
	bool		tgisinternal;
	bool		tgdeferrable;
	bool		tginitdeferred;
	char	   *tgdef;
} TriggerInfo;

typedef struct _evttriggerInfo
{
	DumpableObject dobj;
	char	   *evtname;
	char	   *evtevent;
	char	   *evtowner;
	char	   *evttags;
	char	   *evtfname;
	char		evtenabled;
} EventTriggerInfo;

/*
 * struct ConstraintInfo is used for all constraint types.  However we
 * use a different objType for foreign key constraints, to make it easier
 * to sort them the way we want.
 *
 * Note: condeferrable and condeferred are currently only valid for
 * unique/primary-key constraints.  Otherwise that info is in condef.
 */
typedef struct _constraintInfo
{
	DumpableObject dobj;
	TableInfo  *contable;		/* NULL if domain constraint */
	TypeInfo   *condomain;		/* NULL if table constraint */
	char		contype;
	char	   *condef;			/* definition, if CHECK or FOREIGN KEY */
	Oid			confrelid;		/* referenced table, if FOREIGN KEY */
	DumpId		conindex;		/* identifies associated index if any */
	bool		condeferrable;	/* true if constraint is DEFERRABLE */
	bool		condeferred;	/* true if constraint is INITIALLY DEFERRED */
	bool		conislocal;		/* true if constraint has local definition */
	bool		separate;		/* true if must dump as separate item */
} ConstraintInfo;

typedef struct _procLangInfo
{
	DumpableObject dobj;
	bool		lanpltrusted;
	Oid			lanplcallfoid;
	Oid			laninline;
	Oid			lanvalidator;
	char	   *lanacl;
	char	   *rlanacl;
	char	   *initlanacl;
	char	   *initrlanacl;
	char	   *lanowner;		/* name of owner, or empty string */
} ProcLangInfo;

typedef struct _castInfo
{
	DumpableObject dobj;
	Oid			castsource;
	Oid			casttarget;
	Oid			castfunc;
	char		castcontext;
	char		castmethod;
} CastInfo;

typedef struct _transformInfo
{
	DumpableObject dobj;
	Oid			trftype;
	Oid			trflang;
	Oid			trffromsql;
	Oid			trftosql;
} TransformInfo;

/* InhInfo isn't a DumpableObject, just temporary state */
typedef struct _inhInfo
{
	Oid			inhrelid;		/* OID of a child table */
	Oid			inhparent;		/* OID of its parent */
} InhInfo;

typedef struct _prsInfo
{
	DumpableObject dobj;
	Oid			prsstart;
	Oid			prstoken;
	Oid			prsend;
	Oid			prsheadline;
	Oid			prslextype;
} TSParserInfo;

typedef struct _dictInfo
{
	DumpableObject dobj;
	char	   *rolname;
	Oid			dicttemplate;
	char	   *dictinitoption;
} TSDictInfo;

typedef struct _tmplInfo
{
	DumpableObject dobj;
	Oid			tmplinit;
	Oid			tmpllexize;
} TSTemplateInfo;

typedef struct _cfgInfo
{
	DumpableObject dobj;
	char	   *rolname;
	Oid			cfgparser;
} TSConfigInfo;

typedef struct _fdwInfo
{
	DumpableObject dobj;
	char	   *rolname;
	char	   *fdwhandler;
	char	   *fdwvalidator;
	char	   *fdwoptions;
	char	   *fdwacl;
	char	   *rfdwacl;
	char	   *initfdwacl;
	char	   *initrfdwacl;
} FdwInfo;

typedef struct _foreignServerInfo
{
	DumpableObject dobj;
	char	   *rolname;
	Oid			srvfdw;
	char	   *srvtype;
	char	   *srvversion;
	char	   *srvacl;
	char	   *rsrvacl;
	char	   *initsrvacl;
	char	   *initrsrvacl;
	char	   *srvoptions;
} ForeignServerInfo;

typedef struct _defaultACLInfo
{
	DumpableObject dobj;
	char	   *defaclrole;
	char		defaclobjtype;
	char	   *defaclacl;
	char	   *rdefaclacl;
	char	   *initdefaclacl;
	char	   *initrdefaclacl;
} DefaultACLInfo;

typedef struct _blobInfo
{
	DumpableObject dobj;
	char	   *rolname;
	char	   *blobacl;
	char	   *rblobacl;
	char	   *initblobacl;
	char	   *initrblobacl;
} BlobInfo;

/*
 * The PolicyInfo struct is used to represent policies on a table and
 * to indicate if a table has RLS enabled (ENABLE ROW SECURITY).  If
 * polname is NULL, then the record indicates ENABLE ROW SECURITY, while if
 * it's non-NULL then this is a regular policy definition.
 */
typedef struct _policyInfo
{
	DumpableObject dobj;
	TableInfo  *poltable;
	char	   *polname;		/* null indicates RLS is enabled on rel */
	char		polcmd;
	bool		polpermissive;
	char	   *polroles;
	char	   *polqual;
	char	   *polwithcheck;
} PolicyInfo;

/*
 * The PublicationInfo struct is used to represent publications.
 */
typedef struct _PublicationInfo
{
	DumpableObject dobj;
	char	   *rolname;
	bool		puballtables;
	bool		pubinsert;
	bool		pubupdate;
	bool		pubdelete;
	bool		pubtruncate;
	bool		pubviaroot;
} PublicationInfo;

/*
 * The PublicationRelInfo struct is used to represent publication table
 * mapping.
 */
typedef struct _PublicationRelInfo
{
	DumpableObject dobj;
	PublicationInfo *publication;
	TableInfo  *pubtable;
} PublicationRelInfo;

/*
 * The SubscriptionInfo struct is used to represent subscription.
 */
typedef struct _SubscriptionInfo
{
	DumpableObject dobj;
	char	   *rolname;
	char	   *subconninfo;
	char	   *subslotname;
	char	   *subsynccommit;
	char	   *subpublications;
} SubscriptionInfo;

/*
 * We build an array of these with an entry for each object that is an
 * extension member according to pg_depend.
 */
typedef struct _extensionMemberId
{
	CatalogId	catId;			/* tableoid+oid of some member object */
	ExtensionInfo *ext;			/* owning extension */
} ExtensionMemberId;

/*
 *	common utility functions
 */

extern TableInfo *getSchemaData(Archive *fout, int *numTablesPtr);

extern void AssignDumpId(DumpableObject *dobj);
extern DumpId createDumpId(void);
extern DumpId getMaxDumpId(void);
extern DumpableObject *findObjectByDumpId(DumpId dumpId);
extern DumpableObject *findObjectByCatalogId(CatalogId catalogId);
extern void getDumpableObjects(DumpableObject ***objs, int *numObjs);

extern void addObjectDependency(DumpableObject *dobj, DumpId refId);
extern void removeObjectDependency(DumpableObject *dobj, DumpId refId);

extern TableInfo *findTableByOid(Oid oid);
extern TypeInfo *findTypeByOid(Oid oid);
extern FuncInfo *findFuncByOid(Oid oid);
extern OprInfo *findOprByOid(Oid oid);
extern CollInfo *findCollationByOid(Oid oid);
extern NamespaceInfo *findNamespaceByOid(Oid oid);
extern ExtensionInfo *findExtensionByOid(Oid oid);
extern PublicationInfo *findPublicationByOid(Oid oid);

extern void setExtensionMembership(ExtensionMemberId *extmems, int nextmems);
extern ExtensionInfo *findOwningExtension(CatalogId catalogId);

extern void parseOidArray(const char *str, Oid *array, int arraysize);

extern void sortDumpableObjects(DumpableObject **objs, int numObjs,
								DumpId preBoundaryId, DumpId postBoundaryId);
extern void sortDumpableObjectsByTypeName(DumpableObject **objs, int numObjs);

/*
 * version specific routines
 */
extern NamespaceInfo *getNamespaces(Archive *fout, int *numNamespaces);
extern ExtensionInfo *getExtensions(Archive *fout, int *numExtensions);
extern TypeInfo *getTypes(Archive *fout, int *numTypes);
extern FuncInfo *getFuncs(Archive *fout, int *numFuncs);
extern AggInfo *getAggregates(Archive *fout, int *numAggregates);
extern OprInfo *getOperators(Archive *fout, int *numOperators);
extern AccessMethodInfo *getAccessMethods(Archive *fout, int *numAccessMethods);
extern OpclassInfo *getOpclasses(Archive *fout, int *numOpclasses);
extern OpfamilyInfo *getOpfamilies(Archive *fout, int *numOpfamilies);
extern CollInfo *getCollations(Archive *fout, int *numCollations);
extern ConvInfo *getConversions(Archive *fout, int *numConversions);
extern TableInfo *getTables(Archive *fout, int *numTables);
extern void getOwnedSeqs(Archive *fout, TableInfo tblinfo[], int numTables);
extern InhInfo *getInherits(Archive *fout, int *numInherits);
extern void getPartitioningInfo(Archive *fout);
extern void getIndexes(Archive *fout, TableInfo tblinfo[], int numTables);
extern void getExtendedStatistics(Archive *fout);
extern void getConstraints(Archive *fout, TableInfo tblinfo[], int numTables);
extern RuleInfo *getRules(Archive *fout, int *numRules);
extern void getTriggers(Archive *fout, TableInfo tblinfo[], int numTables);
extern ProcLangInfo *getProcLangs(Archive *fout, int *numProcLangs);
extern CastInfo *getCasts(Archive *fout, int *numCasts);
extern TransformInfo *getTransforms(Archive *fout, int *numTransforms);
extern void getTableAttrs(Archive *fout, TableInfo *tbinfo, int numTables);
extern bool shouldPrintColumn(DumpOptions *dopt, TableInfo *tbinfo, int colno);
extern TSParserInfo *getTSParsers(Archive *fout, int *numTSParsers);
extern TSDictInfo *getTSDictionaries(Archive *fout, int *numTSDicts);
extern TSTemplateInfo *getTSTemplates(Archive *fout, int *numTSTemplates);
extern TSConfigInfo *getTSConfigurations(Archive *fout, int *numTSConfigs);
extern FdwInfo *getForeignDataWrappers(Archive *fout,
									   int *numForeignDataWrappers);
extern ForeignServerInfo *getForeignServers(Archive *fout,
											int *numForeignServers);
extern DefaultACLInfo *getDefaultACLs(Archive *fout, int *numDefaultACLs);
extern void getExtensionMembership(Archive *fout, ExtensionInfo extinfo[],
								   int numExtensions);
extern void processExtensionTables(Archive *fout, ExtensionInfo extinfo[],
								   int numExtensions);
extern EventTriggerInfo *getEventTriggers(Archive *fout, int *numEventTriggers);
extern void getPolicies(Archive *fout, TableInfo tblinfo[], int numTables);
extern PublicationInfo *getPublications(Archive *fout,
										int *numPublications);
extern void getPublicationTables(Archive *fout, TableInfo tblinfo[],
								 int numTables);
extern void getSubscriptions(Archive *fout);

#endif							/* PG_DUMP_H */
