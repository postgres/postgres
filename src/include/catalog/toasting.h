/*-------------------------------------------------------------------------
 *
 * toasting.h
 *	  This file provides some definitions to support creation of toast tables
 *
 * Caution: all #define's with numeric values in this file had better be
 * object OIDs, else renumber_oids.pl might change them inappropriately.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/toasting.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTING_H
#define TOASTING_H

#include "storage/lock.h"

/*
 * toasting.c prototypes
 */
extern void NewRelationCreateToastTable(Oid relOid, Datum reloptions);
extern void NewHeapCreateToastTable(Oid relOid, Datum reloptions,
									LOCKMODE lockmode, Oid OIDOldToast);
extern void AlterTableCreateToastTable(Oid relOid, Datum reloptions,
									   LOCKMODE lockmode);
extern void BootstrapToastTable(char *relName,
								Oid toastOid, Oid toastIndexOid);


/*
 * This macro is just to keep the C compiler from spitting up on the
 * upcoming commands for Catalog.pm.
 */
#define DECLARE_TOAST(name,toastoid,indexoid) extern int no_such_variable


/*
 * What follows are lines processed by genbki.pl to create the statements
 * the bootstrap parser will turn into BootstrapToastTable commands.
 * Each line specifies the system catalog that needs a toast table,
 * the OID to assign to the toast table, and the OID to assign to the
 * toast table's index.  The reason we hard-wire these OIDs is that we
 * need stable OIDs for shared relations, and that includes toast tables
 * of shared relations.
 */

/* normal catalogs */
DECLARE_TOAST(pg_aggregate, 4159, 4160);
DECLARE_TOAST(pg_attrdef, 2830, 2831);
DECLARE_TOAST(pg_collation, 4161, 4162);
DECLARE_TOAST(pg_constraint, 2832, 2833);
DECLARE_TOAST(pg_default_acl, 4143, 4144);
DECLARE_TOAST(pg_description, 2834, 2835);
DECLARE_TOAST(pg_event_trigger, 4145, 4146);
DECLARE_TOAST(pg_extension, 4147, 4148);
DECLARE_TOAST(pg_foreign_data_wrapper, 4149, 4150);
DECLARE_TOAST(pg_foreign_server, 4151, 4152);
DECLARE_TOAST(pg_foreign_table, 4153, 4154);
DECLARE_TOAST(pg_init_privs, 4155, 4156);
DECLARE_TOAST(pg_language, 4157, 4158);
DECLARE_TOAST(pg_namespace, 4163, 4164);
DECLARE_TOAST(pg_partitioned_table, 4165, 4166);
DECLARE_TOAST(pg_policy, 4167, 4168);
DECLARE_TOAST(pg_proc, 2836, 2837);
DECLARE_TOAST(pg_rewrite, 2838, 2839);
DECLARE_TOAST(pg_seclabel, 3598, 3599);
DECLARE_TOAST(pg_statistic, 2840, 2841);
DECLARE_TOAST(pg_statistic_ext, 3439, 3440);
DECLARE_TOAST(pg_statistic_ext_data, 3430, 3431);
DECLARE_TOAST(pg_trigger, 2336, 2337);
DECLARE_TOAST(pg_ts_dict, 4169, 4170);
DECLARE_TOAST(pg_type, 4171, 4172);
DECLARE_TOAST(pg_user_mapping, 4173, 4174);

/* shared catalogs */
DECLARE_TOAST(pg_authid, 4175, 4176);
#define PgAuthidToastTable 4175
#define PgAuthidToastIndex 4176
DECLARE_TOAST(pg_database, 4177, 4178);
#define PgDatabaseToastTable 4177
#define PgDatabaseToastIndex 4178
DECLARE_TOAST(pg_db_role_setting, 2966, 2967);
#define PgDbRoleSettingToastTable 2966
#define PgDbRoleSettingToastIndex 2967
DECLARE_TOAST(pg_pltemplate, 4179, 4180);
#define PgPlTemplateToastTable 4179
#define PgPlTemplateToastIndex 4180
DECLARE_TOAST(pg_replication_origin, 4181, 4182);
#define PgReplicationOriginToastTable 4181
#define PgReplicationOriginToastIndex 4182
DECLARE_TOAST(pg_shdescription, 2846, 2847);
#define PgShdescriptionToastTable 2846
#define PgShdescriptionToastIndex 2847
DECLARE_TOAST(pg_shseclabel, 4060, 4061);
#define PgShseclabelToastTable 4060
#define PgShseclabelToastIndex 4061
DECLARE_TOAST(pg_subscription, 4183, 4184);
#define PgSubscriptionToastTable 4183
#define PgSubscriptionToastIndex 4184
DECLARE_TOAST(pg_tablespace, 4185, 4186);
#define PgTablespaceToastTable 4185
#define PgTablespaceToastIndex 4186

#endif							/* TOASTING_H */
