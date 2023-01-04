/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/syscache.c
 *
 * NOTES
 *	  These routines allow the parser/planner/executor to perform
 *	  rapid lookups on the contents of the system catalogs.
 *
 *	  see utils/syscache.h for a list of the cache IDs
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_range.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "lib/qunique.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*---------------------------------------------------------------------------

	Adding system caches:

	Add your new cache to the list in include/utils/syscache.h.
	Keep the list sorted alphabetically.

	Add your entry to the cacheinfo[] array below. All cache lists are
	alphabetical, so add it in the proper place.  Specify the relation OID,
	index OID, number of keys, key attribute numbers, and initial number of
	hash buckets.

	The number of hash buckets must be a power of 2.  It's reasonable to
	set this to the number of entries that might be in the particular cache
	in a medium-size database.

	There must be a unique index underlying each syscache (ie, an index
	whose key is the same as that of the cache).  If there is not one
	already, add the definition for it to include/catalog/pg_*.h using
	DECLARE_UNIQUE_INDEX.
	(Adding an index requires a catversion.h update, while simply
	adding/deleting caches only requires a recompile.)

	Finally, any place your relation gets heap_insert() or
	heap_update() calls, use CatalogTupleInsert() or CatalogTupleUpdate()
	instead, which also update indexes.  The heap_* calls do not do that.

*---------------------------------------------------------------------------
*/

/*
 *		struct cachedesc: information defining a single syscache
 */
struct cachedesc
{
	Oid			reloid;			/* OID of the relation being cached */
	Oid			indoid;			/* OID of index relation for this cache */
	int			nkeys;			/* # of keys needed for cache lookup */
	int			key[4];			/* attribute numbers of key attrs */
	int			nbuckets;		/* number of hash buckets for this cache */
};

/* Macro to provide nkeys and key array with convenient syntax. */
#define KEY(...) VA_ARGS_NARGS(__VA_ARGS__), { __VA_ARGS__ }

static const struct cachedesc cacheinfo[] = {
	[AGGFNOID] = {
		AggregateRelationId,
		AggregateFnoidIndexId,
		KEY(Anum_pg_aggregate_aggfnoid),
		16
	},
	[AMNAME] = {
		AccessMethodRelationId,
		AmNameIndexId,
		KEY(Anum_pg_am_amname),
		4
	},
	[AMOID] = {
		AccessMethodRelationId,
		AmOidIndexId,
		KEY(Anum_pg_am_oid),
		4
	},
	[AMOPOPID] = {
		AccessMethodOperatorRelationId,
		AccessMethodOperatorIndexId,
		KEY(Anum_pg_amop_amopopr,
			Anum_pg_amop_amoppurpose,
			Anum_pg_amop_amopfamily),
		64
	},
	[AMOPSTRATEGY] = {
		AccessMethodOperatorRelationId,
		AccessMethodStrategyIndexId,
		KEY(Anum_pg_amop_amopfamily,
			Anum_pg_amop_amoplefttype,
			Anum_pg_amop_amoprighttype,
			Anum_pg_amop_amopstrategy),
		64
	},
	[AMPROCNUM] = {
		AccessMethodProcedureRelationId,
		AccessMethodProcedureIndexId,
		KEY(Anum_pg_amproc_amprocfamily,
			Anum_pg_amproc_amproclefttype,
			Anum_pg_amproc_amprocrighttype,
			Anum_pg_amproc_amprocnum),
		16
	},
	[ATTNAME] = {
		AttributeRelationId,
		AttributeRelidNameIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname),
		32
	},
	[ATTNUM] = {
		AttributeRelationId,
		AttributeRelidNumIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum),
		128
	},
	[AUTHMEMMEMROLE] = {
		AuthMemRelationId,
		AuthMemMemRoleIndexId,
		KEY(Anum_pg_auth_members_member,
			Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHMEMROLEMEM] = {
		AuthMemRelationId,
		AuthMemRoleMemIndexId,
		KEY(Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_member,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHNAME] = {
		AuthIdRelationId,
		AuthIdRolnameIndexId,
		KEY(Anum_pg_authid_rolname),
		8
	},
	[AUTHOID] = {
		AuthIdRelationId,
		AuthIdOidIndexId,
		KEY(Anum_pg_authid_oid),
		8
	},
	[CASTSOURCETARGET] = {
		CastRelationId,
		CastSourceTargetIndexId,
		KEY(Anum_pg_cast_castsource,
			Anum_pg_cast_casttarget),
		256
	},
	[CLAAMNAMENSP] = {
		OperatorClassRelationId,
		OpclassAmNameNspIndexId,
		KEY(Anum_pg_opclass_opcmethod,
			Anum_pg_opclass_opcname,
			Anum_pg_opclass_opcnamespace),
		8
	},
	[CLAOID] = {
		OperatorClassRelationId,
		OpclassOidIndexId,
		KEY(Anum_pg_opclass_oid),
		8
	},
	[COLLNAMEENCNSP] = {
		CollationRelationId,
		CollationNameEncNspIndexId,
		KEY(Anum_pg_collation_collname,
			Anum_pg_collation_collencoding,
			Anum_pg_collation_collnamespace),
		8
	},
	[COLLOID] = {
		CollationRelationId,
		CollationOidIndexId,
		KEY(Anum_pg_collation_oid),
		8
	},
	[CONDEFAULT] = {
		ConversionRelationId,
		ConversionDefaultIndexId,
		KEY(Anum_pg_conversion_connamespace,
			Anum_pg_conversion_conforencoding,
			Anum_pg_conversion_contoencoding,
			Anum_pg_conversion_oid),
		8
	},
	[CONNAMENSP] = {
		ConversionRelationId,
		ConversionNameNspIndexId,
		KEY(Anum_pg_conversion_conname,
			Anum_pg_conversion_connamespace),
		8
	},
	[CONSTROID] = {
		ConstraintRelationId,
		ConstraintOidIndexId,
		KEY(Anum_pg_constraint_oid),
		16
	},
	[CONVOID] = {
		ConversionRelationId,
		ConversionOidIndexId,
		KEY(Anum_pg_conversion_oid),
		8
	},
	[DATABASEOID] = {
		DatabaseRelationId,
		DatabaseOidIndexId,
		KEY(Anum_pg_database_oid),
		4
	},
	[DEFACLROLENSPOBJ] = {
		DefaultAclRelationId,
		DefaultAclRoleNspObjIndexId,
		KEY(Anum_pg_default_acl_defaclrole,
			Anum_pg_default_acl_defaclnamespace,
			Anum_pg_default_acl_defaclobjtype),
		8
	},
	[ENUMOID] = {
		EnumRelationId,
		EnumOidIndexId,
		KEY(Anum_pg_enum_oid),
		8
	},
	[ENUMTYPOIDNAME] = {
		EnumRelationId,
		EnumTypIdLabelIndexId,
		KEY(Anum_pg_enum_enumtypid,
			Anum_pg_enum_enumlabel),
		8
	},
	[EVENTTRIGGERNAME] = {
		EventTriggerRelationId,
		EventTriggerNameIndexId,
		KEY(Anum_pg_event_trigger_evtname),
		8
	},
	[EVENTTRIGGEROID] = {
		EventTriggerRelationId,
		EventTriggerOidIndexId,
		KEY(Anum_pg_event_trigger_oid),
		8
	},
	[FOREIGNDATAWRAPPERNAME] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperNameIndexId,
		KEY(Anum_pg_foreign_data_wrapper_fdwname),
		2
	},
	[FOREIGNDATAWRAPPEROID] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperOidIndexId,
		KEY(Anum_pg_foreign_data_wrapper_oid),
		2
	},
	[FOREIGNSERVERNAME] = {
		ForeignServerRelationId,
		ForeignServerNameIndexId,
		KEY(Anum_pg_foreign_server_srvname),
		2
	},
	[FOREIGNSERVEROID] = {
		ForeignServerRelationId,
		ForeignServerOidIndexId,
		KEY(Anum_pg_foreign_server_oid),
		2
	},
	[FOREIGNTABLEREL] = {
		ForeignTableRelationId,
		ForeignTableRelidIndexId,
		KEY(Anum_pg_foreign_table_ftrelid),
		4
	},
	[INDEXRELID] = {
		IndexRelationId,
		IndexRelidIndexId,
		KEY(Anum_pg_index_indexrelid),
		64
	},
	[LANGNAME] = {
		LanguageRelationId,
		LanguageNameIndexId,
		KEY(Anum_pg_language_lanname),
		4
	},
	[LANGOID] = {
		LanguageRelationId,
		LanguageOidIndexId,
		KEY(Anum_pg_language_oid),
		4
	},
	[NAMESPACENAME] = {
		NamespaceRelationId,
		NamespaceNameIndexId,
		KEY(Anum_pg_namespace_nspname),
		4
	},
	[NAMESPACEOID] = {
		NamespaceRelationId,
		NamespaceOidIndexId,
		KEY(Anum_pg_namespace_oid),
		16
	},
	[OPERNAMENSP] = {
		OperatorRelationId,
		OperatorNameNspIndexId,
		KEY(Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprnamespace),
		256
	},
	[OPEROID] = {
		OperatorRelationId,
		OperatorOidIndexId,
		KEY(Anum_pg_operator_oid),
		32
	},
	[OPFAMILYAMNAMENSP] = {
		OperatorFamilyRelationId,
		OpfamilyAmNameNspIndexId,
		KEY(Anum_pg_opfamily_opfmethod,
			Anum_pg_opfamily_opfname,
			Anum_pg_opfamily_opfnamespace),
		8
	},
	[OPFAMILYOID] = {
		OperatorFamilyRelationId,
		OpfamilyOidIndexId,
		KEY(Anum_pg_opfamily_oid),
		8
	},
	[PARAMETERACLNAME] = {
		ParameterAclRelationId,
		ParameterAclParnameIndexId,
		KEY(Anum_pg_parameter_acl_parname),
		4
	},
	[PARAMETERACLOID] = {
		ParameterAclRelationId,
		ParameterAclOidIndexId,
		KEY(Anum_pg_parameter_acl_oid),
		4
	},
	[PARTRELID] = {
		PartitionedRelationId,
		PartitionedRelidIndexId,
		KEY(Anum_pg_partitioned_table_partrelid),
		32
	},
	[PROCNAMEARGSNSP] = {
		ProcedureRelationId,
		ProcedureNameArgsNspIndexId,
		KEY(Anum_pg_proc_proname,
			Anum_pg_proc_proargtypes,
			Anum_pg_proc_pronamespace),
		128
	},
	[PROCOID] = {
		ProcedureRelationId,
		ProcedureOidIndexId,
		KEY(Anum_pg_proc_oid),
		128
	},
	[PUBLICATIONNAME] = {
		PublicationRelationId,
		PublicationNameIndexId,
		KEY(Anum_pg_publication_pubname),
		8
	},
	[PUBLICATIONNAMESPACE] = {
		PublicationNamespaceRelationId,
		PublicationNamespaceObjectIndexId,
		KEY(Anum_pg_publication_namespace_oid),
		64
	},
	[PUBLICATIONNAMESPACEMAP] = {
		PublicationNamespaceRelationId,
		PublicationNamespacePnnspidPnpubidIndexId,
		KEY(Anum_pg_publication_namespace_pnnspid,
			Anum_pg_publication_namespace_pnpubid),
		64
	},
	[PUBLICATIONOID] = {
		PublicationRelationId,
		PublicationObjectIndexId,
		KEY(Anum_pg_publication_oid),
		8
	},
	[PUBLICATIONREL] = {
		PublicationRelRelationId,
		PublicationRelObjectIndexId,
		KEY(Anum_pg_publication_rel_oid),
		64
	},
	[PUBLICATIONRELMAP] = {
		PublicationRelRelationId,
		PublicationRelPrrelidPrpubidIndexId,
		KEY(Anum_pg_publication_rel_prrelid,
			Anum_pg_publication_rel_prpubid),
		64
	},
	[RANGEMULTIRANGE] = {
		RangeRelationId,
		RangeMultirangeTypidIndexId,
		KEY(Anum_pg_range_rngmultitypid),
		4
	},
	[RANGETYPE] = {
		RangeRelationId,
		RangeTypidIndexId,
		KEY(Anum_pg_range_rngtypid),
		4
	},
	[RELNAMENSP] = {
		RelationRelationId,
		ClassNameNspIndexId,
		KEY(Anum_pg_class_relname,
			Anum_pg_class_relnamespace),
		128
	},
	[RELOID] = {
		RelationRelationId,
		ClassOidIndexId,
		KEY(Anum_pg_class_oid),
		128
	},
	[REPLORIGIDENT] = {
		ReplicationOriginRelationId,
		ReplicationOriginIdentIndex,
		KEY(Anum_pg_replication_origin_roident),
		16
	},
	[REPLORIGNAME] = {
		ReplicationOriginRelationId,
		ReplicationOriginNameIndex,
		KEY(Anum_pg_replication_origin_roname),
		16
	},
	[RULERELNAME] = {
		RewriteRelationId,
		RewriteRelRulenameIndexId,
		KEY(Anum_pg_rewrite_ev_class,
			Anum_pg_rewrite_rulename),
		8
	},
	[SEQRELID] = {
		SequenceRelationId,
		SequenceRelidIndexId,
		KEY(Anum_pg_sequence_seqrelid),
		32
	},
	[STATEXTDATASTXOID] = {
		StatisticExtDataRelationId,
		StatisticExtDataStxoidInhIndexId,
		KEY(Anum_pg_statistic_ext_data_stxoid,
			Anum_pg_statistic_ext_data_stxdinherit),
		4
	},
	[STATEXTNAMENSP] = {
		StatisticExtRelationId,
		StatisticExtNameIndexId,
		KEY(Anum_pg_statistic_ext_stxname,
			Anum_pg_statistic_ext_stxnamespace),
		4
	},
	[STATEXTOID] = {
		StatisticExtRelationId,
		StatisticExtOidIndexId,
		KEY(Anum_pg_statistic_ext_oid),
		4
	},
	[STATRELATTINH] = {
		StatisticRelationId,
		StatisticRelidAttnumInhIndexId,
		KEY(Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			Anum_pg_statistic_stainherit),
		128
	},
	[SUBSCRIPTIONNAME] = {
		SubscriptionRelationId,
		SubscriptionNameIndexId,
		KEY(Anum_pg_subscription_subdbid,
			Anum_pg_subscription_subname),
		4
	},
	[SUBSCRIPTIONOID] = {
		SubscriptionRelationId,
		SubscriptionObjectIndexId,
		KEY(Anum_pg_subscription_oid),
		4
	},
	[SUBSCRIPTIONRELMAP] = {
		SubscriptionRelRelationId,
		SubscriptionRelSrrelidSrsubidIndexId,
		KEY(Anum_pg_subscription_rel_srrelid,
			Anum_pg_subscription_rel_srsubid),
		64
	},
	[TABLESPACEOID] = {
		TableSpaceRelationId,
		TablespaceOidIndexId,
		KEY(Anum_pg_tablespace_oid),
		4
	},
	[TRFOID] = {
		TransformRelationId,
		TransformOidIndexId,
		KEY(Anum_pg_transform_oid),
		16
	},
	[TRFTYPELANG] = {
		TransformRelationId,
		TransformTypeLangIndexId,
		KEY(Anum_pg_transform_trftype,
			Anum_pg_transform_trflang),
		16
	},
	[TSCONFIGMAP] = {
		TSConfigMapRelationId,
		TSConfigMapIndexId,
		KEY(Anum_pg_ts_config_map_mapcfg,
			Anum_pg_ts_config_map_maptokentype,
			Anum_pg_ts_config_map_mapseqno),
		2
	},
	[TSCONFIGNAMENSP] = {
		TSConfigRelationId,
		TSConfigNameNspIndexId,
		KEY(Anum_pg_ts_config_cfgname,
			Anum_pg_ts_config_cfgnamespace),
		2
	},
	[TSCONFIGOID] = {
		TSConfigRelationId,
		TSConfigOidIndexId,
		KEY(Anum_pg_ts_config_oid),
		2
	},
	[TSDICTNAMENSP] = {
		TSDictionaryRelationId,
		TSDictionaryNameNspIndexId,
		KEY(Anum_pg_ts_dict_dictname,
			Anum_pg_ts_dict_dictnamespace),
		2
	},
	[TSDICTOID] = {
		TSDictionaryRelationId,
		TSDictionaryOidIndexId,
		KEY(Anum_pg_ts_dict_oid),
		2
	},
	[TSPARSERNAMENSP] = {
		TSParserRelationId,
		TSParserNameNspIndexId,
		KEY(Anum_pg_ts_parser_prsname,
			Anum_pg_ts_parser_prsnamespace),
		2
	},
	[TSPARSEROID] = {
		TSParserRelationId,
		TSParserOidIndexId,
		KEY(Anum_pg_ts_parser_oid),
		2
	},
	[TSTEMPLATENAMENSP] = {
		TSTemplateRelationId,
		TSTemplateNameNspIndexId,
		KEY(Anum_pg_ts_template_tmplname,
			Anum_pg_ts_template_tmplnamespace),
		2
	},
	[TSTEMPLATEOID] = {
		TSTemplateRelationId,
		TSTemplateOidIndexId,
		KEY(Anum_pg_ts_template_oid),
		2
	},
	[TYPENAMENSP] = {
		TypeRelationId,
		TypeNameNspIndexId,
		KEY(Anum_pg_type_typname,
			Anum_pg_type_typnamespace),
		64
	},
	[TYPEOID] = {
		TypeRelationId,
		TypeOidIndexId,
		KEY(Anum_pg_type_oid),
		64
	},
	[USERMAPPINGOID] = {
		UserMappingRelationId,
		UserMappingOidIndexId,
		KEY(Anum_pg_user_mapping_oid),
		2
	},
	[USERMAPPINGUSERSERVER] = {
		UserMappingRelationId,
		UserMappingUserServerIndexId,
		KEY(Anum_pg_user_mapping_umuser,
			Anum_pg_user_mapping_umserver),
		2
	}
};

StaticAssertDecl(lengthof(cacheinfo) == SysCacheSize,
				 "SysCacheSize does not match syscache.c's array");

static CatCache *SysCache[SysCacheSize];

static bool CacheInitialized = false;

/* Sorted array of OIDs of tables that have caches on them */
static Oid	SysCacheRelationOid[SysCacheSize];
static int	SysCacheRelationOidSize;

/* Sorted array of OIDs of tables and indexes used by caches */
static Oid	SysCacheSupportingRelOid[SysCacheSize * 2];
static int	SysCacheSupportingRelOidSize;

static int	oid_compare(const void *a, const void *b);


/*
 * InitCatalogCache - initialize the caches
 *
 * Note that no database access is done here; we only allocate memory
 * and initialize the cache structure.  Interrogation of the database
 * to complete initialization of a cache happens upon first use
 * of that cache.
 */
void
InitCatalogCache(void)
{
	int			cacheId;

	Assert(!CacheInitialized);

	SysCacheRelationOidSize = SysCacheSupportingRelOidSize = 0;

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
	{
		/*
		 * Assert that every enumeration value defined in syscache.h has been
		 * populated in the cacheinfo array.
		 */
		Assert(cacheinfo[cacheId].reloid != 0);

		SysCache[cacheId] = InitCatCache(cacheId,
										 cacheinfo[cacheId].reloid,
										 cacheinfo[cacheId].indoid,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key,
										 cacheinfo[cacheId].nbuckets);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR, "could not initialize cache %u (%d)",
				 cacheinfo[cacheId].reloid, cacheId);
		/* Accumulate data for OID lists, too */
		SysCacheRelationOid[SysCacheRelationOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].indoid;
		/* see comments for RelationInvalidatesSnapshotsOnly */
		Assert(!RelationInvalidatesSnapshotsOnly(cacheinfo[cacheId].reloid));
	}

	Assert(SysCacheRelationOidSize <= lengthof(SysCacheRelationOid));
	Assert(SysCacheSupportingRelOidSize <= lengthof(SysCacheSupportingRelOid));

	/* Sort and de-dup OID arrays, so we can use binary search. */
	pg_qsort(SysCacheRelationOid, SysCacheRelationOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheRelationOidSize =
		qunique(SysCacheRelationOid, SysCacheRelationOidSize, sizeof(Oid),
				oid_compare);

	pg_qsort(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheSupportingRelOidSize =
		qunique(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
				sizeof(Oid), oid_compare);

	CacheInitialized = true;
}

/*
 * InitCatalogCachePhase2 - finish initializing the caches
 *
 * Finish initializing all the caches, including necessary database
 * access.
 *
 * This is *not* essential; normally we allow syscaches to be initialized
 * on first use.  However, it is useful as a mechanism to preload the
 * relcache with entries for the most-commonly-used system catalogs.
 * Therefore, we invoke this routine when we need to write a new relcache
 * init file.
 */
void
InitCatalogCachePhase2(void)
{
	int			cacheId;

	Assert(CacheInitialized);

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
		InitCatCachePhase2(SysCache[cacheId], true);
}


/*
 * SearchSysCache
 *
 *	A layer on top of SearchCatCache that does the initialization and
 *	key-setting for you.
 *
 *	Returns the cache copy of the tuple if one is found, NULL if not.
 *	The tuple is the 'cache' copy and must NOT be modified!
 *
 *	When the caller is done using the tuple, call ReleaseSysCache()
 *	to release the reference count grabbed by SearchSysCache().  If this
 *	is not done, the tuple will remain locked in cache until end of
 *	transaction, which is tolerable but not desirable.
 *
 *	CAUTION: The tuple that is returned must NOT be freed by the caller!
 */
HeapTuple
SearchSysCache(int cacheId,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));

	return SearchCatCache(SysCache[cacheId], key1, key2, key3, key4);
}

HeapTuple
SearchSysCache1(int cacheId,
				Datum key1)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 1);

	return SearchCatCache1(SysCache[cacheId], key1);
}

HeapTuple
SearchSysCache2(int cacheId,
				Datum key1, Datum key2)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 2);

	return SearchCatCache2(SysCache[cacheId], key1, key2);
}

HeapTuple
SearchSysCache3(int cacheId,
				Datum key1, Datum key2, Datum key3)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 3);

	return SearchCatCache3(SysCache[cacheId], key1, key2, key3);
}

HeapTuple
SearchSysCache4(int cacheId,
				Datum key1, Datum key2, Datum key3, Datum key4)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 4);

	return SearchCatCache4(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * ReleaseSysCache
 *		Release previously grabbed reference count on a tuple
 */
void
ReleaseSysCache(HeapTuple tuple)
{
	ReleaseCatCache(tuple);
}

/*
 * SearchSysCacheCopy
 *
 * A convenience routine that does SearchSysCache and (if successful)
 * returns a modifiable copy of the syscache entry.  The original
 * syscache entry is released before returning.  The caller should
 * heap_freetuple() the result when done with it.
 */
HeapTuple
SearchSysCacheCopy(int cacheId,
				   Datum key1,
				   Datum key2,
				   Datum key3,
				   Datum key4)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExists
 *
 * A convenience routine that just probes to see if a tuple can be found.
 * No lock is retained on the syscache entry.
 */
bool
SearchSysCacheExists(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}

/*
 * GetSysCacheOid
 *
 * A convenience routine that does SearchSysCache and returns the OID in the
 * oidcol column of the found tuple, or InvalidOid if no tuple could be found.
 * No lock is retained on the syscache entry.
 */
Oid
GetSysCacheOid(int cacheId,
			   AttrNumber oidcol,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	HeapTuple	tuple;
	bool		isNull;
	Oid			result;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;
	result = heap_getattr(tuple, oidcol,
						  SysCache[cacheId]->cc_tupdesc,
						  &isNull);
	Assert(!isNull);			/* columns used as oids should never be NULL */
	ReleaseSysCache(tuple);
	return result;
}


/*
 * SearchSysCacheAttName
 *
 * This routine is equivalent to SearchSysCache on the ATTNAME cache,
 * except that it will return NULL if the found attribute is marked
 * attisdropped.  This is convenient for callers that want to act as
 * though dropped attributes don't exist.
 */
HeapTuple
SearchSysCacheAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNAME,
							ObjectIdGetDatum(relid),
							CStringGetDatum(attname));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

/*
 * SearchSysCacheCopyAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheCopy.
 */
HeapTuple
SearchSysCacheCopyAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExistsAttName
 *
 * As above, an attisdropped-aware version of SearchSysCacheExists.
 */
bool
SearchSysCacheExistsAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}


/*
 * SearchSysCacheAttNum
 *
 * This routine is equivalent to SearchSysCache on the ATTNUM cache,
 * except that it will return NULL if the found attribute is marked
 * attisdropped.  This is convenient for callers that want to act as
 * though dropped attributes don't exist.
 */
HeapTuple
SearchSysCacheAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNUM,
							ObjectIdGetDatum(relid),
							Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

/*
 * SearchSysCacheCopyAttNum
 *
 * As above, an attisdropped-aware version of SearchSysCacheCopy.
 */
HeapTuple
SearchSysCacheCopyAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttNum(relid, attnum);
	if (!HeapTupleIsValid(tuple))
		return NULL;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}


/*
 * SysCacheGetAttr
 *
 *		Given a tuple previously fetched by SearchSysCache(),
 *		extract a specific attribute.
 *
 * This is equivalent to using heap_getattr() on a tuple fetched
 * from a non-cached relation.  Usually, this is only used for attributes
 * that could be NULL or variable length; the fixed-size attributes in
 * a system table are accessed just by mapping the tuple onto the C struct
 * declarations from include/catalog/.
 *
 * As with heap_getattr(), if the attribute is of a pass-by-reference type
 * then a pointer into the tuple data area is returned --- the caller must
 * not modify or pfree the datum!
 *
 * Note: it is legal to use SysCacheGetAttr() with a cacheId referencing
 * a different cache for the same catalog the tuple was fetched from.
 */
Datum
SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber,
				bool *isNull)
{
	/*
	 * We just need to get the TupleDesc out of the cache entry, and then we
	 * can apply heap_getattr().  Normally the cache control data is already
	 * valid (because the caller recently fetched the tuple via this same
	 * cache), but there are cases where we have to initialize the cache here.
	 */
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);
	if (!PointerIsValid(SysCache[cacheId]->cc_tupdesc))
	{
		InitCatCachePhase2(SysCache[cacheId], false);
		Assert(PointerIsValid(SysCache[cacheId]->cc_tupdesc));
	}

	return heap_getattr(tup, attributeNumber,
						SysCache[cacheId]->cc_tupdesc,
						isNull);
}

/*
 * GetSysCacheHashValue
 *
 * Get the hash value that would be used for a tuple in the specified cache
 * with the given search keys.
 *
 * The reason for exposing this as part of the API is that the hash value is
 * exposed in cache invalidation operations, so there are places outside the
 * catcache code that need to be able to compute the hash values.
 */
uint32
GetSysCacheHashValue(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);

	return GetCatCacheHashValue(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * List-search interface
 */
struct catclist *
SearchSysCacheList(int cacheId, int nkeys,
				   Datum key1, Datum key2, Datum key3)
{
	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);

	return SearchCatCacheList(SysCache[cacheId], nkeys,
							  key1, key2, key3);
}

/*
 * SysCacheInvalidate
 *
 *	Invalidate entries in the specified cache, given a hash value.
 *	See CatCacheInvalidate() for more info.
 *
 *	This routine is only quasi-public: it should only be used by inval.c.
 */
void
SysCacheInvalidate(int cacheId, uint32 hashValue)
{
	if (cacheId < 0 || cacheId >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheId);

	/* if this cache isn't initialized yet, no need to do anything */
	if (!PointerIsValid(SysCache[cacheId]))
		return;

	CatCacheInvalidate(SysCache[cacheId], hashValue);
}

/*
 * Certain relations that do not have system caches send snapshot invalidation
 * messages in lieu of catcache messages.  This is for the benefit of
 * GetCatalogSnapshot(), which can then reuse its existing MVCC snapshot
 * for scanning one of those catalogs, rather than taking a new one, if no
 * invalidation has been received.
 *
 * Relations that have syscaches need not (and must not) be listed here.  The
 * catcache invalidation messages will also flush the snapshot.  If you add a
 * syscache for one of these relations, remove it from this list.
 */
bool
RelationInvalidatesSnapshotsOnly(Oid relid)
{
	switch (relid)
	{
		case DbRoleSettingRelationId:
		case DependRelationId:
		case SharedDependRelationId:
		case DescriptionRelationId:
		case SharedDescriptionRelationId:
		case SecLabelRelationId:
		case SharedSecLabelRelationId:
			return true;
		default:
			break;
	}

	return false;
}

/*
 * Test whether a relation has a system cache.
 */
bool
RelationHasSysCache(Oid relid)
{
	int			low = 0,
				high = SysCacheRelationOidSize - 1;

	while (low <= high)
	{
		int			middle = low + (high - low) / 2;

		if (SysCacheRelationOid[middle] == relid)
			return true;
		if (SysCacheRelationOid[middle] < relid)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return false;
}

/*
 * Test whether a relation supports a system cache, ie it is either a
 * cached table or the index used for a cache.
 */
bool
RelationSupportsSysCache(Oid relid)
{
	int			low = 0,
				high = SysCacheSupportingRelOidSize - 1;

	while (low <= high)
	{
		int			middle = low + (high - low) / 2;

		if (SysCacheSupportingRelOid[middle] == relid)
			return true;
		if (SysCacheSupportingRelOid[middle] < relid)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return false;
}


/*
 * OID comparator for pg_qsort
 */
static int
oid_compare(const void *a, const void *b)
{
	Oid			oa = *((const Oid *) a);
	Oid			ob = *((const Oid *) b);

	if (oa == ob)
		return 0;
	return (oa > ob) ? 1 : -1;
}
