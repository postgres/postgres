/*
 * SQL Information Schema
 * as defined in ISO 9075-2:1999 chapter 20
 *
 * Copyright 2003, PostgreSQL Global Development Group
 *
 * $Id: information_schema.sql,v 1.15.2.5 2003/12/17 22:11:42 tgl Exp $
 */

/*
 * Note: Generally, the definitions in this file should be ordered
 * according to the clause numbers in the SQL standard, which is also the
 * alphabetical order.  In some cases it is convenient or necessary to
 * define one information schema view by using another one; in that case,
 * put the referencing view at the very end and leave a note where it
 * should have been put.
 */


/*
 * 20.2
 * INFORMATION_SCHEMA schema
 */

CREATE SCHEMA information_schema;
GRANT USAGE ON SCHEMA information_schema TO PUBLIC;
SET search_path TO information_schema, public;


-- 20.3 INFORMATION_SCHEMA_CATALOG_NAME view appears later.


/*
 * 20.4
 * CARDINAL_NUMBER domain
 */

CREATE DOMAIN cardinal_number AS integer
    CONSTRAINT cardinal_number_domain_check CHECK (value >= 0);


/*
 * 20.5
 * CHARACTER_DATA domain
 */

CREATE DOMAIN character_data AS character varying;


/*
 * 20.6
 * SQL_IDENTIFIER domain
 */

CREATE DOMAIN sql_identifier AS character varying;


/*
 * 20.3
 * INFORMATION_SCHEMA_CATALOG_NAME view
 */

CREATE VIEW information_schema_catalog_name AS
    SELECT CAST(current_database() AS sql_identifier) AS catalog_name;

GRANT SELECT ON information_schema_catalog_name TO PUBLIC;


/*
 * 20.7
 * TIME_STAMP domain
 */

CREATE DOMAIN time_stamp AS timestamp(2)
    DEFAULT current_timestamp(2);


/*
 * 20.9
 * APPLICABLE_ROLES view
 */

CREATE VIEW applicable_roles AS
    SELECT CAST(current_user AS sql_identifier) AS grantee,
           CAST(g.groname AS sql_identifier) AS role_name,
           CAST('NO' AS character_data) AS is_grantable

    FROM pg_group g, pg_user u

    WHERE u.usesysid = ANY (g.grolist)
          AND u.usename = current_user;

GRANT SELECT ON applicable_roles TO PUBLIC;


/*
 * 20.13
 * CHECK_CONSTRAINTS view
 */

CREATE VIEW check_constraints AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(rs.nspname AS sql_identifier) AS constraint_schema,
           CAST(con.conname AS sql_identifier) AS constraint_name,
           CAST(substring(pg_get_constraintdef(con.oid) from 7) AS character_data)
             AS check_clause
    FROM pg_namespace rs,
         pg_constraint con
           LEFT OUTER JOIN pg_class c ON (c.oid = con.conrelid)
           LEFT OUTER JOIN pg_type t ON (t.oid = con.contypid),
         pg_user u
    WHERE rs.oid = con.connamespace
          AND u.usesysid = coalesce(c.relowner, t.typowner)
          AND u.usename = current_user
          AND con.contype = 'c';

GRANT SELECT ON check_constraints TO PUBLIC;


/*
 * 20.15
 * COLUMN_DOMAIN_USAGE view
 */

CREATE VIEW column_domain_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS domain_catalog,
           CAST(nt.nspname AS sql_identifier) AS domain_schema,
           CAST(t.typname AS sql_identifier) AS domain_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name

    FROM pg_type t, pg_namespace nt, pg_class c, pg_namespace nc,
         pg_attribute a, pg_user u

    WHERE t.typnamespace = nt.oid
          AND c.relnamespace = nc.oid
          AND a.attrelid = c.oid
          AND a.atttypid = t.oid
          AND t.typowner = u.usesysid
          AND t.typtype = 'd'
          AND c.relkind IN ('r', 'v')
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND u.usename = current_user;

GRANT SELECT ON column_domain_usage TO PUBLIC;


/*
 * 20.16
 * COLUMN_PRIVILEGES
 */

CREATE VIEW column_privileges AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(grantee.name AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST(pr.type AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(c.relacl,
                                   makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, pr.type, true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable

    FROM pg_attribute a,
         pg_class c,
         pg_namespace nc,
         pg_user u_grantor,
         (
           SELECT usesysid, 0, usename FROM pg_user
           UNION ALL
           SELECT 0, grosysid, groname FROM pg_group
           UNION ALL
           SELECT 0, 0, 'PUBLIC'
         ) AS grantee (usesysid, grosysid, name),
         (SELECT 'SELECT' UNION ALL
          SELECT 'INSERT' UNION ALL
          SELECT 'UPDATE' UNION ALL
          SELECT 'REFERENCES') AS pr (type)

    WHERE a.attrelid = c.oid
          AND c.relnamespace = nc.oid
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND c.relkind IN ('r', 'v')
          AND aclcontains(c.relacl,
                          makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, pr.type, false))
          AND (u_grantor.usename = current_user
               OR grantee.name = current_user
               OR grantee.name = 'PUBLIC');

GRANT SELECT ON column_privileges TO PUBLIC;


/*
 * 20.17
 * COLUMN_UDT_USAGE view
 */

CREATE VIEW column_udt_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(coalesce(nbt.nspname, nt.nspname) AS sql_identifier) AS udt_schema,
           CAST(coalesce(bt.typname, t.typname) AS sql_identifier) AS udt_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name

    FROM pg_attribute a, pg_class c, pg_namespace nc, pg_user u,
         (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid))
           LEFT JOIN (pg_type bt JOIN pg_namespace nbt ON (bt.typnamespace = nbt.oid))
           ON (t.typtype = 'd' AND t.typbasetype = bt.oid)

    WHERE a.attrelid = c.oid
          AND a.atttypid = t.oid
          AND u.usesysid = coalesce(bt.typowner, t.typowner)
          AND nc.oid = c.relnamespace
          AND a.attnum > 0 AND NOT a.attisdropped AND c.relkind in ('r', 'v')
          AND u.usename = current_user;

GRANT SELECT ON column_udt_usage TO PUBLIC;


/*
 * 20.18
 * COLUMNS view
 */

CREATE VIEW columns AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST(a.attnum AS cardinal_number) AS ordinal_position,
           CAST(
             CASE WHEN u.usename = current_user THEN a.adsrc ELSE null END
             AS character_data)
             AS column_default,
           CAST(CASE WHEN a.attnotnull OR (t.typtype = 'd' AND t.typnotnull) THEN 'NO' ELSE 'YES' END
             AS character_data)
             AS is_nullable,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN bt.typelem <> 0 AND bt.typlen = -1 THEN 'ARRAY'
                    WHEN nbt.nspname = 'pg_catalog' THEN format_type(t.typbasetype, null)
                    ELSE 'USER-DEFINED' END
             ELSE
               CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
                    WHEN nt.nspname = 'pg_catalog' THEN format_type(a.atttypid, null)
                    ELSE 'USER-DEFINED' END
             END
             AS character_data)
             AS data_type,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN t.typbasetype IN (1042, 1043) AND t.typtypmod <> -1
                    THEN t.typtypmod - 4 /* char, varchar */
                    WHEN t.typbasetype IN (1560, 1562) AND t.typtypmod <> -1
                    THEN t.typtypmod /* bit, varbit */
                    ELSE null END
             ELSE
               CASE WHEN a.atttypid IN (1042, 1043) AND a.atttypmod <> -1
                    THEN a.atttypmod - 4
                    WHEN a.atttypid IN (1560, 1562) AND a.atttypmod <> -1
                    THEN a.atttypmod
                    ELSE null END
             END
             AS cardinal_number)
             AS character_maximum_length,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN t.typbasetype IN (25, 1042, 1043) THEN 2^30 ELSE null END
             ELSE
               CASE WHEN a.atttypid IN (25, 1042, 1043) THEN 2^30 ELSE null END
             END
             AS cardinal_number)
             AS character_octet_length,

           CAST(
             CASE (CASE WHEN t.typtype = 'd' THEN t.typbasetype ELSE a.atttypid END)
               WHEN 21 /*int2*/ THEN 16
               WHEN 23 /*int4*/ THEN 32
               WHEN 20 /*int8*/ THEN 64
               WHEN 1700 /*numeric*/ THEN ((CASE WHEN t.typtype = 'd' THEN t.typtypmod ELSE a.atttypmod END - 4) >> 16) & 65535
               WHEN 700 /*float4*/ THEN 24 /*FLT_MANT_DIG*/
               WHEN 701 /*float8*/ THEN 53 /*DBL_MANT_DIG*/
               ELSE null END
             AS cardinal_number)
             AS numeric_precision,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN t.typbasetype IN (21, 23, 20, 700, 701) THEN 2
                    WHEN t.typbasetype IN (1700) THEN 10
                    ELSE null END
             ELSE
               CASE WHEN a.atttypid IN (21, 23, 20, 700, 701) THEN 2
                    WHEN a.atttypid IN (1700) THEN 10
                    ELSE null END
             END
             AS cardinal_number)
             AS numeric_precision_radix,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN t.typbasetype IN (21, 23, 20) THEN 0
                    WHEN t.typbasetype IN (1700) THEN (t.typtypmod - 4) & 65535
                    ELSE null END
             ELSE
               CASE WHEN a.atttypid IN (21, 23, 20) THEN 0
                    WHEN a.atttypid IN (1700) THEN (a.atttypmod - 4) & 65535
                    ELSE null END
             END
             AS cardinal_number)
             AS numeric_scale,

           CAST(
             CASE WHEN t.typtype = 'd' THEN
               CASE WHEN t.typbasetype IN (1083, 1114, 1184, 1266)
                    THEN (CASE WHEN t.typtypmod <> -1 THEN t.typtypmod ELSE null END)
                    WHEN t.typbasetype IN (1186)
                    THEN (CASE WHEN t.typtypmod <> -1 THEN t.typtypmod & 65535 ELSE null END)
                    ELSE null END
             ELSE
               CASE WHEN a.atttypid IN (1083, 1114, 1184, 1266)
                    THEN (CASE WHEN a.atttypmod <> -1 THEN a.atttypmod ELSE null END)
                    WHEN a.atttypid IN (1186)
                    THEN (CASE WHEN a.atttypmod <> -1 THEN a.atttypmod & 65535 ELSE null END)
                    ELSE null END
             END
             AS cardinal_number)
             AS datetime_precision,

           CAST(null AS character_data) AS interval_type, -- XXX
           CAST(null AS character_data) AS interval_precision, -- XXX

           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,

           CAST(null AS sql_identifier) AS collation_catalog,
           CAST(null AS sql_identifier) AS collation_schema,
           CAST(null AS sql_identifier) AS collation_name,

           CAST(CASE WHEN t.typtype = 'd' THEN current_database() ELSE null END
             AS sql_identifier) AS domain_catalog,
           CAST(CASE WHEN t.typtype = 'd' THEN nt.nspname ELSE null END
             AS sql_identifier) AS domain_schema,
           CAST(CASE WHEN t.typtype = 'd' THEN t.typname ELSE null END
             AS sql_identifier) AS domain_name,

           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(coalesce(nbt.nspname, nt.nspname) AS sql_identifier) AS udt_schema,
           CAST(coalesce(bt.typname, t.typname) AS sql_identifier) AS udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(a.attnum AS sql_identifier) AS dtd_identifier,
           CAST('NO' AS character_data) AS is_self_referencing

    FROM (pg_attribute LEFT JOIN pg_attrdef ON attrelid = adrelid AND attnum = adnum) AS a,
         pg_class c, pg_namespace nc, pg_user u,
         (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid))
           LEFT JOIN (pg_type bt JOIN pg_namespace nbt ON (bt.typnamespace = nbt.oid))
           ON (t.typtype = 'd' AND t.typbasetype = bt.oid)

    WHERE a.attrelid = c.oid
          AND a.atttypid = t.oid
          AND u.usesysid = c.relowner
          AND nc.oid = c.relnamespace

          AND a.attnum > 0 AND NOT a.attisdropped AND c.relkind in ('r', 'v')

          AND (u.usename = current_user
               OR has_table_privilege(c.oid, 'SELECT')
               OR has_table_privilege(c.oid, 'INSERT')
               OR has_table_privilege(c.oid, 'UPDATE')
               OR has_table_privilege(c.oid, 'REFERENCES') );

GRANT SELECT ON columns TO PUBLIC;


/*
 * 20.19
 * CONSTRAINT_COLUMN_USAGE view
 */

/* This returns the integers from 1 to INDEX_MAX_KEYS/FUNC_MAX_ARGS */
CREATE FUNCTION _pg_keypositions() RETURNS SETOF integer
    LANGUAGE sql
    IMMUTABLE
    AS 'select 1 union all select 2 union all select 3 union all
        select 4 union all select 5 union all select 6 union all
        select 7 union all select 8 union all select 9 union all
        select 10 union all select 11 union all select 12 union all
        select 13 union all select 14 union all select 15 union all
        select 16 union all select 17 union all select 18 union all
        select 19 union all select 20 union all select 21 union all
        select 22 union all select 23 union all select 24 union all
        select 25 union all select 26 union all select 27 union all
        select 28 union all select 29 union all select 30 union all
        select 31 union all select 32';

CREATE VIEW constraint_column_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(tblschema AS sql_identifier) AS table_schema,
           CAST(tblname AS sql_identifier) AS table_name,
           CAST(colname AS sql_identifier) AS column_name,
           CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(cstrschema AS sql_identifier) AS constraint_schema,
           CAST(cstrname AS sql_identifier) AS constraint_name

    FROM (
        /* check constraints */
        SELECT DISTINCT nr.nspname, r.relname, r.relowner, a.attname, nc.nspname, c.conname
          FROM pg_namespace nr, pg_class r, pg_attribute a, pg_depend d, pg_namespace nc, pg_constraint c
          WHERE nr.oid = r.relnamespace
            AND r.oid = a.attrelid
            AND d.refclassid = 'pg_catalog.pg_class'::regclass
            AND d.refobjid = r.oid
            AND d.refobjsubid = a.attnum
            AND d.classid = 'pg_catalog.pg_constraint'::regclass
            AND d.objid = c.oid
            AND c.connamespace = nc.oid
            AND c.contype = 'c'
            AND r.relkind = 'r'
            AND NOT a.attisdropped

        UNION ALL

        /* unique/primary key/foreign key constraints */
        SELECT nr.nspname, r.relname, r.relowner, a.attname, nc.nspname, c.conname
          FROM pg_namespace nr, pg_class r, pg_attribute a, pg_namespace nc,
               pg_constraint c, _pg_keypositions() AS pos(n)
          WHERE nr.oid = r.relnamespace
            AND r.oid = a.attrelid
            AND nc.oid = c.connamespace
            AND (CASE WHEN c.contype = 'f' THEN r.oid = c.confrelid AND c.confkey[pos.n] = a.attnum
                      ELSE r.oid = c.conrelid AND c.conkey[pos.n] = a.attnum END)
            AND NOT a.attisdropped
            AND c.contype IN ('p', 'u', 'f')
            AND r.relkind = 'r'

      ) AS x (tblschema, tblname, tblowner, colname, cstrschema, cstrname),
      pg_user u

    WHERE x.tblowner = u.usesysid AND u.usename = current_user;

GRANT SELECT ON constraint_column_usage TO PUBLIC;


/*
 * 20.20
 * CONSTRAINT_TABLE_USAGE view
 */

CREATE VIEW constraint_table_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nr.nspname AS sql_identifier) AS table_schema,
           CAST(r.relname AS sql_identifier) AS table_name,
           CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nc.nspname AS sql_identifier) AS constraint_schema,
           CAST(c.conname AS sql_identifier) AS constraint_name

    FROM pg_constraint c, pg_namespace nc,
         pg_class r, pg_namespace nr,
         pg_user u

    WHERE c.connamespace = nc.oid AND r.relnamespace = nr.oid
          AND ( (c.contype = 'f' AND c.confrelid = r.oid)
             OR (c.contype IN ('p', 'u') AND c.conrelid = r.oid) )
          AND r.relkind = 'r'
          AND r.relowner = u.usesysid AND u.usename = current_user;

GRANT SELECT ON constraint_table_usage TO PUBLIC;


-- 20.21 DATA_TYPE_PRIVILEGES view appears later.


/*
 * 20.24
 * DOMAIN_CONSTRAINTS view
 */

CREATE VIEW domain_constraints AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(rs.nspname AS sql_identifier) AS constraint_schema,
           CAST(con.conname AS sql_identifier) AS constraint_name,
           CAST(current_database() AS sql_identifier) AS domain_catalog,
           CAST(n.nspname AS sql_identifier) AS domain_schema,
           CAST(t.typname AS sql_identifier) AS domain_name,
           CAST(CASE WHEN condeferrable THEN 'YES' ELSE 'NO' END
             AS character_data) AS is_deferrable,
           CAST(CASE WHEN condeferred THEN 'YES' ELSE 'NO' END
             AS character_data) AS initially_deferred
    FROM pg_namespace rs, pg_namespace n, pg_constraint con, pg_type t, pg_user u
    WHERE rs.oid = con.connamespace
          AND n.oid = t.typnamespace
          AND u.usesysid = t.typowner
          AND u.usename = current_user
          AND t.oid = con.contypid;

GRANT SELECT ON domain_constraints TO PUBLIC;


/*
 * 20.25
 * DOMAIN_UDT_USAGE view
 */

CREATE VIEW domain_udt_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,
           CAST(current_database() AS sql_identifier) AS domain_catalog,
           CAST(nt.nspname AS sql_identifier) AS domain_schema,
           CAST(t.typname AS sql_identifier) AS domain_name

    FROM pg_type t, pg_namespace nt,
         pg_type bt, pg_namespace nbt,
         pg_user u

    WHERE t.typnamespace = nt.oid
          AND t.typbasetype = bt.oid
          AND bt.typnamespace = nbt.oid
          AND t.typtype = 'd'
          AND bt.typowner = u.usesysid
          AND u.usename = current_user;

GRANT SELECT ON domain_udt_usage TO PUBLIC;


/*
 * 20.26
 * DOMAINS view
 */

CREATE VIEW domains AS
    SELECT CAST(current_database() AS sql_identifier) AS domain_catalog,
           CAST(nt.nspname AS sql_identifier) AS domain_schema,
           CAST(t.typname AS sql_identifier) AS domain_name,

           CAST(
             CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
                  WHEN nbt.nspname = 'pg_catalog' THEN format_type(t.typbasetype, null)
                  ELSE 'USER-DEFINED' END
             AS character_data)
             AS data_type,

           CAST(
             CASE WHEN t.typbasetype IN (1042, 1043) AND t.typtypmod <> -1
                  THEN t.typtypmod - 4 /* char, varchar */
                  WHEN t.typbasetype IN (1560, 1562) AND t.typtypmod <> -1
                  THEN t.typtypmod /* bit, varbit */
                  ELSE null END
             AS cardinal_number)
             AS character_maximum_length,

           CAST(
             CASE WHEN t.typbasetype IN (25, 1042, 1043) THEN 2^30 ELSE null END
             AS cardinal_number)
             AS character_octet_length,

           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,

           CAST(null AS sql_identifier) AS collation_catalog,
           CAST(null AS sql_identifier) AS collation_schema,
           CAST(null AS sql_identifier) AS collation_name,

           CAST(
             CASE t.typbasetype
               WHEN 21 /*int2*/ THEN 16
               WHEN 23 /*int4*/ THEN 32
               WHEN 20 /*int8*/ THEN 64
               WHEN 1700 /*numeric*/ THEN ((t.typtypmod - 4) >> 16) & 65535
               WHEN 700 /*float4*/ THEN 24 /*FLT_MANT_DIG*/
               WHEN 701 /*float8*/ THEN 53 /*DBL_MANT_DIG*/
               ELSE null END
             AS cardinal_number)
             AS numeric_precision,

           CAST(
             CASE WHEN t.typbasetype IN (21, 23, 20, 700, 701) THEN 2
                  WHEN t.typbasetype IN (1700) THEN 10
                  ELSE null END
             AS cardinal_number)
             AS numeric_precision_radix,

           CAST(
             CASE WHEN t.typbasetype IN (21, 23, 20) THEN 0
                  WHEN t.typbasetype IN (1700) THEN (t.typtypmod - 4) & 65535
                  ELSE null END
             AS cardinal_number)
             AS numeric_scale,

           CAST(
             CASE WHEN t.typbasetype IN (1083, 1114, 1184, 1266)
                  THEN (CASE WHEN t.typtypmod <> -1 THEN t.typtypmod ELSE null END)
                  WHEN t.typbasetype IN (1186)
                  THEN (CASE WHEN t.typtypmod <> -1 THEN t.typtypmod & 65535 ELSE null END)
                  ELSE null END
             AS cardinal_number)
             AS datetime_precision,

           CAST(null AS character_data) AS interval_type, -- XXX
           CAST(null AS character_data) AS interval_precision, -- XXX

           CAST(t.typdefault AS character_data) AS domain_default,

           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(1 AS sql_identifier) AS dtd_identifier

    FROM pg_type t, pg_namespace nt,
         pg_type bt, pg_namespace nbt

    WHERE t.typnamespace = nt.oid
          AND t.typbasetype = bt.oid
          AND bt.typnamespace = nbt.oid
          AND t.typtype = 'd';

GRANT SELECT ON domains TO PUBLIC;


-- 20.27 ELEMENT_TYPES view appears later.


/*
 * 20.28
 * ENABLED_ROLES view
 */

CREATE VIEW enabled_roles AS
    SELECT CAST(g.groname AS sql_identifier) AS role_name
    FROM pg_group g, pg_user u
    WHERE u.usesysid = ANY (g.grolist)
          AND u.usename = current_user;

GRANT SELECT ON enabled_roles TO PUBLIC;


/*
 * 20.30
 * KEY_COLUMN_USAGE view
 */

CREATE VIEW key_column_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nc.nspname AS sql_identifier) AS constraint_schema,
           CAST(c.conname AS sql_identifier) AS constraint_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nr.nspname AS sql_identifier) AS table_schema,
           CAST(r.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST(pos.n AS cardinal_number) AS ordinal_position

    FROM pg_namespace nr, pg_class r, pg_attribute a, pg_namespace nc,
         pg_constraint c, pg_user u, _pg_keypositions() AS pos(n)
    WHERE nr.oid = r.relnamespace
          AND r.oid = a.attrelid
          AND r.oid = c.conrelid
          AND nc.oid = c.connamespace
          AND c.conkey[pos.n] = a.attnum
          AND NOT a.attisdropped
          AND c.contype IN ('p', 'u', 'f')
          AND r.relkind = 'r'
          AND r.relowner = u.usesysid
          AND u.usename = current_user;

GRANT SELECT ON key_column_usage TO PUBLIC;


/*
 * 20.33
 * PARAMETERS view
 */

CREATE VIEW parameters AS
    SELECT CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier) AS specific_name,
           CAST(pos.n AS cardinal_number) AS ordinal_position,
           CAST('IN' AS character_data) AS parameter_mode,
           CAST('NO' AS character_data) AS is_result,
           CAST('NO' AS character_data) AS as_locator,
           CAST(null AS sql_identifier) AS parameter_name,
           CAST(
             CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
                  WHEN nt.nspname = 'pg_catalog' THEN format_type(t.oid, null)
                  ELSE 'USER-DEFINED' END AS character_data)
             AS data_type,
           CAST(null AS cardinal_number) AS character_maximum_length,
           CAST(null AS cardinal_number) AS character_octet_length,
           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,
           CAST(null AS sql_identifier) AS collation_catalog,
           CAST(null AS sql_identifier) AS collation_schema,
           CAST(null AS sql_identifier) AS collation_name,
           CAST(null AS cardinal_number) AS numeric_precision,
           CAST(null AS cardinal_number) AS numeric_precision_radix,
           CAST(null AS cardinal_number) AS numeric_scale,
           CAST(null AS cardinal_number) AS datetime_precision,
           CAST(null AS character_data) AS interval_type,
           CAST(null AS character_data) AS interval_precision,
           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS udt_schema,
           CAST(t.typname AS sql_identifier) AS udt_name,
           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,
           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(pos.n AS sql_identifier) AS dtd_identifier

    FROM pg_namespace n, pg_proc p, pg_type t, pg_namespace nt, pg_user u,
         _pg_keypositions() AS pos(n)

    WHERE n.oid = p.pronamespace AND p.pronargs >= pos.n
          AND p.proargtypes[pos.n-1] = t.oid AND t.typnamespace = nt.oid
          AND p.proowner = u.usesysid
          AND (u.usename = current_user OR has_function_privilege(p.oid, 'EXECUTE'));

GRANT SELECT ON parameters TO PUBLIC;


/*
 * 20.35
 * REFERENTIAL_CONSTRAINTS view
 */

CREATE FUNCTION _pg_keyissubset(smallint[], smallint[]) RETURNS boolean
    LANGUAGE sql
    IMMUTABLE
    RETURNS NULL ON NULL INPUT
    AS 'select $1[1] is null or ($1[1] = any ($2) and coalesce(information_schema._pg_keyissubset($1[2:pg_catalog.array_upper($1,1)], $2), true))';

CREATE FUNCTION _pg_keysequal(smallint[], smallint[]) RETURNS boolean
    LANGUAGE sql
    IMMUTABLE
    RETURNS NULL ON NULL INPUT
    AS 'select information_schema._pg_keyissubset($1, $2) and information_schema._pg_keyissubset($2, $1)';

CREATE VIEW referential_constraints AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(ncon.nspname AS sql_identifier) AS constraint_schema,
           CAST(con.conname AS sql_identifier) AS constraint_name,
           CAST(
             CASE WHEN npkc.nspname IS NULL THEN NULL
                  ELSE current_database() END
             AS sql_identifier) AS unique_constraint_catalog,
           CAST(npkc.nspname AS sql_identifier) AS unique_constraint_schema,
           CAST(pkc.conname AS sql_identifier) AS unique_constraint_name,

           CAST(
             CASE con.confmatchtype WHEN 'f' THEN 'FULL'
                                    WHEN 'p' THEN 'PARTIAL'
                                    WHEN 'u' THEN 'NONE' END
             AS character_data) AS match_option,

           CAST(
             CASE con.confupdtype WHEN 'c' THEN 'CASCADE'
                                  WHEN 'n' THEN 'SET NULL'
                                  WHEN 'd' THEN 'SET DEFAULT'
                                  WHEN 'r' THEN 'RESTRICT'
                                  WHEN 'a' THEN 'NO ACTION' END
             AS character_data) AS update_rule,

           CAST(
             CASE con.confdeltype WHEN 'c' THEN 'CASCADE'
                                  WHEN 'n' THEN 'SET NULL'
                                  WHEN 'd' THEN 'SET DEFAULT'
                                  WHEN 'r' THEN 'RESTRICT'
                                  WHEN 'a' THEN 'NO ACTION' END
             AS character_data) AS delete_rule

    FROM (pg_namespace ncon INNER JOIN pg_constraint con ON ncon.oid = con.connamespace
         INNER JOIN pg_class c ON con.conrelid = c.oid
         INNER JOIN pg_user u ON c.relowner = u.usesysid)
         LEFT JOIN
         (pg_constraint pkc INNER JOIN pg_namespace npkc ON pkc.connamespace = npkc.oid)
         ON con.confrelid = pkc.conrelid AND _pg_keysequal(con.confkey, pkc.conkey)

    WHERE c.relkind = 'r'
          AND con.contype = 'f'
          AND (pkc.contype IN ('p', 'u') OR pkc.contype IS NULL)
          AND u.usename = current_user;

GRANT SELECT ON referential_constraints TO PUBLIC;


/*
 * 20.36
 * ROLE_COLUMN_GRANTS view
 */

CREATE VIEW role_column_grants AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(g_grantee.groname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST(pr.type AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(c.relacl,
                                   makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, pr.type, true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable

    FROM pg_attribute a,
         pg_class c,
         pg_namespace nc,
         pg_user u_grantor,
         pg_group g_grantee,
         (SELECT 'SELECT' UNION ALL
          SELECT 'INSERT' UNION ALL
          SELECT 'UPDATE' UNION ALL
          SELECT 'REFERENCES') AS pr (type)

    WHERE a.attrelid = c.oid
          AND c.relnamespace = nc.oid
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND c.relkind IN ('r', 'v')
          AND aclcontains(c.relacl,
                          makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, pr.type, false))
          AND g_grantee.groname IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_column_grants TO PUBLIC;


/*
 * 20.37
 * ROLE_ROUTINE_GRANTS view
 */

CREATE VIEW role_routine_grants AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(g_grantee.groname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier) AS specific_name,
           CAST(current_database() AS sql_identifier) AS routine_catalog,
           CAST(n.nspname AS sql_identifier) AS routine_schema,
           CAST(p.proname AS sql_identifier) AS routine_name,
           CAST('EXECUTE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(p.proacl,
                                   makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, 'EXECUTE', true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable

    FROM pg_proc p,
         pg_namespace n,
         pg_user u_grantor,
         pg_group g_grantee

    WHERE p.pronamespace = n.oid
          AND aclcontains(p.proacl,
                          makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, 'EXECUTE', false))
          AND g_grantee.groname IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_routine_grants TO PUBLIC;


/*
 * 20.38
 * ROLE_TABLE_GRANTS view
 */

CREATE VIEW role_table_grants AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(g_grantee.groname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(pr.type AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(c.relacl,
                                   makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, pr.type, true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable,
           CAST('NO' AS character_data) AS with_hierarchy

    FROM pg_class c,
         pg_namespace nc,
         pg_user u_grantor,
         pg_group g_grantee,
         (SELECT 'SELECT' UNION ALL
          SELECT 'DELETE' UNION ALL
          SELECT 'INSERT' UNION ALL
          SELECT 'UPDATE' UNION ALL
          SELECT 'REFERENCES' UNION ALL
          SELECT 'RULE' UNION ALL
          SELECT 'TRIGGER') AS pr (type)

    WHERE c.relnamespace = nc.oid
          AND c.relkind IN ('r', 'v')
          AND aclcontains(c.relacl,
                          makeaclitem(0, g_grantee.grosysid, u_grantor.usesysid, pr.type, false))
          AND g_grantee.groname IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_table_grants TO PUBLIC;


/*
 * 20.40
 * ROLE_USAGE_GRANTS view
 */

-- See USAGE_PRIVILEGES.

CREATE VIEW role_usage_grants AS
    SELECT CAST(null AS sql_identifier) AS grantor,
           CAST(null AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(null AS sql_identifier) AS object_schema,
           CAST(null AS sql_identifier) AS object_name,
           CAST(null AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST(null AS character_data) AS is_grantable

    WHERE false;

GRANT SELECT ON role_usage_grants TO PUBLIC;


/*
 * 20.43
 * ROUTINE_PRIVILEGES view
 */

CREATE VIEW routine_privileges AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(grantee.name AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier) AS specific_name,
           CAST(current_database() AS sql_identifier) AS routine_catalog,
           CAST(n.nspname AS sql_identifier) AS routine_schema,
           CAST(p.proname AS sql_identifier) AS routine_name,
           CAST('EXECUTE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(p.proacl,
                                   makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, 'EXECUTE', true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable

    FROM pg_proc p,
         pg_namespace n,
         pg_user u_grantor,
         (
           SELECT usesysid, 0, usename FROM pg_user
           UNION ALL
           SELECT 0, grosysid, groname FROM pg_group
           UNION ALL
           SELECT 0, 0, 'PUBLIC'
         ) AS grantee (usesysid, grosysid, name)

    WHERE p.pronamespace = n.oid
          AND aclcontains(p.proacl,
                          makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, 'EXECUTE', false))
          AND (u_grantor.usename = current_user
               OR grantee.name = current_user
               OR grantee.name = 'PUBLIC');

GRANT SELECT ON routine_privileges TO PUBLIC;


/*
 * 20.45
 * ROUTINES view
 */

CREATE VIEW routines AS
    SELECT CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier) AS specific_name,
           CAST(current_database() AS sql_identifier) AS routine_catalog,
           CAST(n.nspname AS sql_identifier) AS routine_schema,
           CAST(p.proname AS sql_identifier) AS routine_name,
           CAST('FUNCTION' AS character_data) AS routine_type,
           CAST(null AS sql_identifier) AS module_catalog,
           CAST(null AS sql_identifier) AS module_schema,
           CAST(null AS sql_identifier) AS module_name,
           CAST(null AS sql_identifier) AS udt_catalog,
           CAST(null AS sql_identifier) AS udt_schema,
           CAST(null AS sql_identifier) AS udt_name,

           CAST(
             CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
                  WHEN nt.nspname = 'pg_catalog' THEN format_type(t.oid, null)
                  ELSE 'USER-DEFINED' END AS character_data)
             AS data_type,
           CAST(null AS cardinal_number) AS character_maximum_length,
           CAST(null AS cardinal_number) AS character_octet_length,
           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,
           CAST(null AS sql_identifier) AS collation_catalog,
           CAST(null AS sql_identifier) AS collation_schema,
           CAST(null AS sql_identifier) AS collation_name,
           CAST(null AS cardinal_number) AS numeric_precision,
           CAST(null AS cardinal_number) AS numeric_precision_radix,
           CAST(null AS cardinal_number) AS numeric_scale,
           CAST(null AS cardinal_number) AS datetime_precision,
           CAST(null AS character_data) AS interval_type,
           CAST(null AS character_data) AS interval_precision,
           CAST(current_database() AS sql_identifier) AS type_udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS type_udt_schema,
           CAST(t.typname AS sql_identifier) AS type_udt_name,
           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,
           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(0 AS sql_identifier) AS dtd_identifier,

           CAST(CASE WHEN l.lanname = 'sql' THEN 'SQL' ELSE 'EXTERNAL' END AS character_data)
             AS routine_body,
           CAST(
             CASE WHEN u.usename = current_user THEN p.prosrc ELSE null END
             AS character_data) AS routine_definition,
           CAST(
             CASE WHEN l.lanname = 'c' THEN p.prosrc ELSE null END
             AS character_data) AS external_name,
           CAST(upper(l.lanname) AS character_data) AS external_language,

           CAST('GENERAL' AS character_data) AS parameter_style,
           CAST(CASE WHEN p.provolatile = 'i' THEN 'YES' ELSE 'NO' END AS character_data) AS is_deterministic,
           CAST('MODIFIES' AS character_data) AS sql_data_access,
           CAST(CASE WHEN p.proisstrict THEN 'YES' ELSE 'NO' END AS character_data) AS is_null_call,
           CAST(null AS character_data) AS sql_path,
           CAST('YES' AS character_data) AS schema_level_routine,
           CAST(0 AS cardinal_number) AS max_dynamic_result_sets,
           CAST(null AS character_data) AS is_user_defined_cast,
           CAST(null AS character_data) AS is_implicitly_invocable,
           CAST(CASE WHEN p.prosecdef THEN 'DEFINER' ELSE 'INVOKER' END AS character_data) AS security_type,
           CAST(null AS sql_identifier) AS to_sql_specific_catalog,
           CAST(null AS sql_identifier) AS to_sql_specific_schema,
           CAST(null AS sql_identifier) AS to_sql_specific_name,
           CAST('NO' AS character_data) AS as_locator

    FROM pg_namespace n, pg_proc p, pg_language l, pg_user u,
         pg_type t, pg_namespace nt

    WHERE n.oid = p.pronamespace AND p.prolang = l.oid AND p.proowner = u.usesysid
          AND p.prorettype = t.oid AND t.typnamespace = nt.oid
          AND (u.usename = current_user OR has_function_privilege(p.oid, 'EXECUTE'));

GRANT SELECT ON routines TO PUBLIC;


/*
 * 20.46
 * SCHEMATA view
 */

CREATE VIEW schemata AS
    SELECT CAST(current_database() AS sql_identifier) AS catalog_name,
           CAST(n.nspname AS sql_identifier) AS schema_name,
           CAST(u.usename AS sql_identifier) AS schema_owner,
           CAST(null AS sql_identifier) AS default_character_set_catalog,
           CAST(null AS sql_identifier) AS default_character_set_schema,
           CAST(null AS sql_identifier) AS default_character_set_name,
           CAST(null AS character_data) AS sql_path
    FROM pg_namespace n, pg_user u
    WHERE n.nspowner = u.usesysid AND u.usename = current_user;

GRANT SELECT ON schemata TO PUBLIC;


/*
 * 20.47
 * SQL_FEATURES table
 */

CREATE TABLE sql_features (
    feature_id          character_data,
    feature_name        character_data,
    sub_feature_id      character_data,
    sub_feature_name    character_data,
    is_supported        character_data,
    is_verified_by      character_data,
    comments            character_data
) WITHOUT OIDS;

-- Will be filled with external data by initdb.

GRANT SELECT ON sql_features TO PUBLIC;


/*
 * 20.48
 * SQL_IMPLEMENTATION_INFO table
 */

-- Note: Implementation information items are defined in ISO 9075-3:1999,
-- clause 7.1.

CREATE TABLE sql_implementation_info (
    implementation_info_id      character_data,
    implementation_info_name    character_data,
    integer_value               cardinal_number,
    character_value             character_data,
    comments                    character_data
) WITHOUT OIDS;

INSERT INTO sql_implementation_info VALUES ('10003', 'CATALOG NAME', NULL, 'Y', NULL);
INSERT INTO sql_implementation_info VALUES ('10004', 'COLLATING SEQUENCE', NULL, '', 'not supported');
INSERT INTO sql_implementation_info VALUES ('23',    'CURSOR COMMIT BEHAVIOR', 1, NULL, 'close cursors and retain prepared statements');
INSERT INTO sql_implementation_info VALUES ('2',     'DATA SOURCE NAME', NULL, '', NULL);
INSERT INTO sql_implementation_info VALUES ('17',    'DBMS NAME', NULL, (select trim(trailing ' ' from substring(version() from '^[^0-9]*'))), NULL);
INSERT INTO sql_implementation_info VALUES ('18',    'DBMS VERSION', NULL, '???', NULL); -- filled by initdb
INSERT INTO sql_implementation_info VALUES ('26',    'DEFAULT TRANSACTION ISOLATION', 2, NULL, 'READ COMMITTED; user-settable');
INSERT INTO sql_implementation_info VALUES ('28',    'IDENTIFIER CASE', 3, NULL, 'stored in mixed case - case sensitive');
INSERT INTO sql_implementation_info VALUES ('85',    'NULL COLLATION', 0, NULL, 'nulls higher than non-nulls');
INSERT INTO sql_implementation_info VALUES ('13',    'SERVER NAME', NULL, '', NULL);
INSERT INTO sql_implementation_info VALUES ('94',    'SPECIAL CHARACTERS', NULL, '', 'all non-ASCII characters allowed');
INSERT INTO sql_implementation_info VALUES ('46',    'TRANSACTION CAPABLE', 2, NULL, 'both DML and DDL');

GRANT SELECT ON sql_implementation_info TO PUBLIC;


/*
 * 20.49
 * SQL_LANGUAGES table
 */

CREATE TABLE sql_languages (
    sql_language_source         character_data,
    sql_language_year           character_data,
    sql_language_conformance    character_data,
    sql_language_integrity      character_data,
    sql_language_implementation character_data,
    sql_language_binding_style  character_data,
    sql_language_programming_language character_data
) WITHOUT OIDS;

INSERT INTO sql_languages VALUES ('ISO 9075', '1999', 'CORE', NULL, NULL, 'DIRECT', NULL);
INSERT INTO sql_languages VALUES ('ISO 9075', '1999', 'CORE', NULL, NULL, 'EMBEDDED', 'C');

GRANT SELECT ON sql_languages TO PUBLIC;


/*
 * 20.50
 * SQL_PACKAGES table
 */

CREATE TABLE sql_packages (
    feature_id      character_data,
    feature_name    character_data,
    is_supported    character_data,
    is_verified_by  character_data,
    comments        character_data
) WITHOUT OIDS;

INSERT INTO sql_packages VALUES ('PKG000', 'Core', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG001', 'Enhanced datetime facilities', 'YES', NULL, '');
INSERT INTO sql_packages VALUES ('PKG002', 'Enhanced integrity management', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG003', 'OLAP facilities', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG004', 'PSM', 'NO', NULL, 'PL/pgSQL is similar.');
INSERT INTO sql_packages VALUES ('PKG005', 'CLI', 'NO', NULL, 'ODBC is similar.');
INSERT INTO sql_packages VALUES ('PKG006', 'Basic object support', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG007', 'Enhanced object support', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG008', 'Active database', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG009', 'SQL/MM support', 'NO', NULL, '');

GRANT SELECT ON sql_packages TO PUBLIC;


/*
 * 20.51
 * SQL_SIZING table
 */

-- Note: Sizing items are defined in ISO 9075-3:1999, clause 7.2.

CREATE TABLE sql_sizing (
    sizing_id       cardinal_number,
    sizing_name     character_data,
    supported_value cardinal_number,
    comments        character_data
) WITHOUT OIDS;

INSERT INTO sql_sizing VALUES (34,    'MAXIMUM CATALOG NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (30,    'MAXIMUM COLUMN NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (97,    'MAXIMUM COLUMNS IN GROUP BY', 0, NULL);
INSERT INTO sql_sizing VALUES (99,    'MAXIMUM COLUMNS IN ORDER BY', 0, NULL);
INSERT INTO sql_sizing VALUES (100,   'MAXIMUM COLUMNS IN SELECT', 1664, NULL); -- match MaxTupleAttributeNumber
INSERT INTO sql_sizing VALUES (101,   'MAXIMUM COLUMNS IN TABLE', 1600, NULL); -- match MaxHeapAttributeNumber
INSERT INTO sql_sizing VALUES (1,     'MAXIMUM CONCURRENT ACTIVITIES', 0, NULL);
INSERT INTO sql_sizing VALUES (31,    'MAXIMUM CURSOR NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (0,     'MAXIMUM DRIVER CONNECTIONS', NULL, NULL);
INSERT INTO sql_sizing VALUES (10005, 'MAXIMUM IDENTIFIER LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (32,    'MAXIMUM SCHEMA NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (20000, 'MAXIMUM STATEMENT OCTETS', 0, NULL);
INSERT INTO sql_sizing VALUES (20001, 'MAXIMUM STATEMENT OCTETS DATA', 0, NULL);
INSERT INTO sql_sizing VALUES (20002, 'MAXIMUM STATEMENT OCTETS SCHEMA', 0, NULL);
INSERT INTO sql_sizing VALUES (35,    'MAXIMUM TABLE NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (106,   'MAXIMUM TABLES IN SELECT', 0, NULL);
INSERT INTO sql_sizing VALUES (107,   'MAXIMUM USER NAME LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (25000, 'MAXIMUM CURRENT DEFAULT TRANSFORM GROUP LENGTH', NULL, NULL);
INSERT INTO sql_sizing VALUES (25001, 'MAXIMUM CURRENT TRANSFORM GROUP LENGTH', NULL, NULL);
INSERT INTO sql_sizing VALUES (25002, 'MAXIMUM CURRENT PATH LENGTH', 0, NULL);
INSERT INTO sql_sizing VALUES (25003, 'MAXIMUM CURRENT ROLE LENGTH', NULL, NULL);
INSERT INTO sql_sizing VALUES (25004, 'MAXIMUM SESSION USER LENGTH', 63, NULL);
INSERT INTO sql_sizing VALUES (25005, 'MAXIMUM SYSTEM USER LENGTH', 63, NULL);

UPDATE sql_sizing
    SET supported_value = (SELECT typlen-1 FROM pg_catalog.pg_type WHERE typname = 'name'),
        comments = 'Might be less, depending on character set.'
    WHERE supported_value = 63;

GRANT SELECT ON sql_sizing TO PUBLIC;


/*
 * 20.52
 * SQL_SIZING_PROFILES table
 */

-- The data in this table are defined by various profiles of SQL.
-- Since we don't have any information about such profiles, we provide
-- an empty table.

CREATE TABLE sql_sizing_profiles (
    sizing_id       cardinal_number,
    sizing_name     character_data,
    profile_id      character_data,
    required_value  cardinal_number,
    comments        character_data
) WITHOUT OIDS;

GRANT SELECT ON sql_sizing_profiles TO PUBLIC;


/*
 * 20.53
 * TABLE_CONSTRAINTS view
 */

CREATE VIEW table_constraints AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nc.nspname AS sql_identifier) AS constraint_schema,
           CAST(c.conname AS sql_identifier) AS constraint_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nr.nspname AS sql_identifier) AS table_schema,
           CAST(r.relname AS sql_identifier) AS table_name,
           CAST(
             CASE c.contype WHEN 'c' THEN 'CHECK'
                            WHEN 'f' THEN 'FOREIGN KEY'
                            WHEN 'p' THEN 'PRIMARY KEY'
                            WHEN 'u' THEN 'UNIQUE' END
             AS character_data) AS constraint_type,
           CAST(CASE WHEN c.condeferrable THEN 'YES' ELSE 'NO' END AS character_data)
             AS is_deferrable,
           CAST(CASE WHEN c.condeferred THEN 'YES' ELSE 'NO' END AS character_data)
             AS initially_deferred

    FROM pg_namespace nc,
         pg_namespace nr,
         pg_constraint c,
         pg_class r,
         pg_user u

    WHERE nc.oid = c.connamespace AND nr.oid = r.relnamespace
          AND c.conrelid = r.oid AND r.relowner = u.usesysid
          AND r.relkind = 'r'
          AND u.usename = current_user;

-- FIMXE: Not-null constraints are missing here.

GRANT SELECT ON table_constraints TO PUBLIC;


/*
 * 20.55
 * TABLE_PRIVILEGES view
 */

CREATE VIEW table_privileges AS
    SELECT CAST(u_grantor.usename AS sql_identifier) AS grantor,
           CAST(grantee.name AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(pr.type AS character_data) AS privilege_type,
           CAST(
             CASE WHEN aclcontains(c.relacl,
                                   makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, pr.type, true))
                  THEN 'YES' ELSE 'NO' END AS character_data) AS is_grantable,
           CAST('NO' AS character_data) AS with_hierarchy

    FROM pg_class c,
         pg_namespace nc,
         pg_user u_grantor,
         (
           SELECT usesysid, 0, usename FROM pg_user
           UNION ALL
           SELECT 0, grosysid, groname FROM pg_group
           UNION ALL
           SELECT 0, 0, 'PUBLIC'
         ) AS grantee (usesysid, grosysid, name),
         (SELECT 'SELECT' UNION ALL
          SELECT 'DELETE' UNION ALL
          SELECT 'INSERT' UNION ALL
          SELECT 'UPDATE' UNION ALL
          SELECT 'REFERENCES' UNION ALL
          SELECT 'RULE' UNION ALL
          SELECT 'TRIGGER') AS pr (type)

    WHERE c.relnamespace = nc.oid
          AND c.relkind IN ('r', 'v')
          AND aclcontains(c.relacl,
                          makeaclitem(grantee.usesysid, grantee.grosysid, u_grantor.usesysid, pr.type, false))
          AND (u_grantor.usename = current_user
               OR grantee.name = current_user
               OR grantee.name = 'PUBLIC');

GRANT SELECT ON table_privileges TO PUBLIC;


/*
 * 20.56
 * TABLES view
 */

CREATE VIEW tables AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,

           CAST(
             CASE WHEN nc.nspname LIKE 'pg!_temp!_%' ESCAPE '!' THEN 'LOCAL TEMPORARY'
                  WHEN c.relkind = 'r' THEN 'BASE TABLE'
                  WHEN c.relkind = 'v' THEN 'VIEW'
                  ELSE null END
             AS character_data) AS table_type,

           CAST(null AS sql_identifier) AS self_referencing_column_name,
           CAST(null AS character_data) AS reference_generation,

           CAST(null AS sql_identifier) AS user_defined_type_catalog,
           CAST(null AS sql_identifier) AS user_defined_type_schema,
           CAST(null AS sql_identifier) AS user_defined_name

    FROM pg_namespace nc, pg_class c, pg_user u

    WHERE c.relnamespace = nc.oid AND u.usesysid = c.relowner
          AND c.relkind IN ('r', 'v')
          AND (u.usename = current_user
               OR has_table_privilege(c.oid, 'SELECT')
               OR has_table_privilege(c.oid, 'INSERT')
               OR has_table_privilege(c.oid, 'UPDATE')
               OR has_table_privilege(c.oid, 'DELETE')
               OR has_table_privilege(c.oid, 'RULE')
               OR has_table_privilege(c.oid, 'REFERENCES')
               OR has_table_privilege(c.oid, 'TRIGGER') );

GRANT SELECT ON tables TO PUBLIC;


/*
 * 20.59
 * TRIGGERED_UPDATE_COLUMNS view
 */

-- PostgreSQL doesn't allow the specification of individual triggered
-- update columns, so this view is empty.

CREATE VIEW triggered_update_columns AS
    SELECT CAST(current_database() AS sql_identifier) AS trigger_catalog,
           CAST(null AS sql_identifier) AS trigger_schema,
           CAST(null AS sql_identifier) AS trigger_name,
           CAST(current_database() AS sql_identifier) AS event_object_catalog,
           CAST(null AS sql_identifier) AS event_object_schema,
           CAST(null AS sql_identifier) AS event_object_table,
           CAST(null AS sql_identifier) AS event_object_column
    WHERE false;

GRANT SELECT ON triggered_update_columns TO PUBLIC;


/*
 * 20.62
 * TRIGGERS view
 */

CREATE VIEW triggers AS
    SELECT CAST(current_database() AS sql_identifier) AS trigger_catalog,
           CAST(n.nspname AS sql_identifier) AS trigger_schema,
           CAST(t.tgname AS sql_identifier) AS trigger_name,
           CAST(em.text AS character_data) AS event_manipulation,
           CAST(current_database() AS sql_identifier) AS event_object_catalog,
           CAST(n.nspname AS sql_identifier) AS event_object_schema,
           CAST(c.relname AS sql_identifier) AS event_object_table,
           CAST(null AS cardinal_number) AS action_order,
           CAST(null AS character_data) AS action_condition,
           CAST(
             substring(pg_get_triggerdef(t.oid) from
                       position('EXECUTE PROCEDURE' in substring(pg_get_triggerdef(t.oid) from 48)) + 47)
             AS character_data) AS action_statement,
           CAST(
             CASE WHEN t.tgtype & 1 = 1 THEN 'ROW' ELSE 'STATEMENT' END
             AS character_data) AS action_orientation,
           CAST(
             CASE WHEN t.tgtype & 2 = 2 THEN 'BEFORE' ELSE 'AFTER' END
             AS character_data) AS condition_timing,
           CAST(null AS sql_identifier) AS condition_reference_old_table,
           CAST(null AS sql_identifier) AS condition_reference_new_table

    FROM pg_namespace n, pg_class c, pg_trigger t, pg_user u,
         (SELECT 4, 'INSERT' UNION ALL
          SELECT 8, 'DELETE' UNION ALL
          SELECT 16, 'UPDATE') AS em (num, text)

    WHERE n.oid = c.relnamespace
          AND c.oid = t.tgrelid
          AND c.relowner = u.usesysid
          AND t.tgtype & em.num <> 0
          AND NOT t.tgisconstraint
          AND u.usename = current_user;

GRANT SELECT ON triggers TO PUBLIC;


/*
 * 20.63
 * USAGE_PRIVILEGES view
 */

-- Of the things currently implemented in PostgreSQL, usage privileges
-- apply only to domains.  Since domains have no real privileges, we
-- represent all domains with implicit usage privilege here.

CREATE VIEW usage_privileges AS
    SELECT CAST(u.usename AS sql_identifier) AS grantor,
           CAST('PUBLIC' AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(t.typname AS sql_identifier) AS object_name,
           CAST('DOMAIN' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST('NO' AS character_data) AS is_grantable

    FROM pg_user u,
         pg_namespace n,
         pg_type t

    WHERE u.usesysid = t.typowner
          AND t.typnamespace = n.oid
          AND t.typtype = 'd';

GRANT SELECT ON usage_privileges TO PUBLIC;


/*
 * 20.65
 * VIEW_COLUMN_USAGE
 */

CREATE VIEW view_column_usage AS
    SELECT DISTINCT
           CAST(current_database() AS sql_identifier) AS view_catalog,
           CAST(nv.nspname AS sql_identifier) AS view_schema,
           CAST(v.relname AS sql_identifier) AS view_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nt.nspname AS sql_identifier) AS table_schema,
           CAST(t.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name

    FROM pg_user, pg_namespace nv, pg_class v, pg_depend dv,
         pg_depend dt, pg_class t, pg_namespace nt,
         pg_attribute a, pg_user u

    WHERE nv.oid = v.relnamespace
          AND v.relkind = 'v'
          AND v.oid = dv.refobjid
          AND dv.refclassid = 'pg_catalog.pg_class'::regclass
          AND dv.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dv.deptype = 'i'
          AND dv.objid = dt.objid
          AND dv.refobjid <> dt.refobjid
          AND dt.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dt.refclassid = 'pg_catalog.pg_class'::regclass
          AND dt.refobjid = t.oid
          AND t.relnamespace = nt.oid
          AND t.relkind IN ('r', 'v')
          AND t.oid = a.attrelid
          AND dt.refobjsubid = a.attnum
          AND t.relowner = u.usesysid AND u.usename = current_user;

GRANT SELECT ON view_column_usage TO PUBLIC;


/*
 * 20.66
 * VIEW_TABLE_USAGE
 */

CREATE VIEW view_table_usage AS
    SELECT DISTINCT
           CAST(current_database() AS sql_identifier) AS view_catalog,
           CAST(nv.nspname AS sql_identifier) AS view_schema,
           CAST(v.relname AS sql_identifier) AS view_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nt.nspname AS sql_identifier) AS table_schema,
           CAST(t.relname AS sql_identifier) AS table_name

    FROM pg_user, pg_namespace nv, pg_class v, pg_depend dv,
         pg_depend dt, pg_class t, pg_namespace nt,
         pg_user u

    WHERE nv.oid = v.relnamespace
          AND v.relkind = 'v'
          AND v.oid = dv.refobjid
          AND dv.refclassid = 'pg_catalog.pg_class'::regclass
          AND dv.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dv.deptype = 'i'
          AND dv.objid = dt.objid
          AND dv.refobjid <> dt.refobjid
          AND dt.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dt.refclassid = 'pg_catalog.pg_class'::regclass
          AND dt.refobjid = t.oid
          AND t.relnamespace = nt.oid
          AND t.relkind IN ('r', 'v')
          AND t.relowner = u.usesysid AND u.usename = current_user;

GRANT SELECT ON view_table_usage TO PUBLIC;


/*
 * 20.68
 * VIEWS view
 */

CREATE VIEW views AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,

           CAST(
             CASE WHEN u.usename = current_user THEN pg_get_viewdef(c.oid)
                  ELSE null END
             AS character_data) AS view_definition,

           CAST('NONE' AS character_data) AS check_option,
           CAST(null AS character_data) AS is_updatable, -- FIXME
           CAST(null AS character_data) AS is_insertable_into  -- FIXME

    FROM pg_namespace nc, pg_class c, pg_user u

    WHERE c.relnamespace = nc.oid AND u.usesysid = c.relowner
          AND c.relkind = 'v'
          AND (u.usename = current_user
               OR has_table_privilege(c.oid, 'SELECT')
               OR has_table_privilege(c.oid, 'INSERT')
               OR has_table_privilege(c.oid, 'UPDATE')
               OR has_table_privilege(c.oid, 'DELETE')
               OR has_table_privilege(c.oid, 'RULE')
               OR has_table_privilege(c.oid, 'REFERENCES')
               OR has_table_privilege(c.oid, 'TRIGGER') );

GRANT SELECT ON views TO PUBLIC;


-- The following views have dependencies that force them to appear out of order.

/*
 * 20.21
 * DATA_TYPE_PRIVILEGES view
 */

CREATE VIEW data_type_privileges AS
    SELECT CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(x.objschema AS sql_identifier) AS object_schema,
           CAST(x.objname AS sql_identifier) AS object_name,
           CAST(x.objtype AS character_data) AS object_type,
           CAST(x.objdtdid AS sql_identifier) AS dtd_identifier

    FROM
      (
        SELECT table_schema, table_name, 'TABLE'::text, dtd_identifier FROM columns
        UNION ALL
        SELECT domain_schema, domain_name, 'DOMAIN'::text, dtd_identifier FROM domains
        UNION ALL
        SELECT specific_schema, specific_name, 'ROUTINE'::text, dtd_identifier FROM parameters
        UNION ALL
        SELECT specific_schema, specific_name, 'ROUTINE'::text, dtd_identifier FROM routines
      ) AS x (objschema, objname, objtype, objdtdid);

GRANT SELECT ON data_type_privileges TO PUBLIC;


/*
 * 20.27
 * ELEMENT_TYPES view
 */

CREATE VIEW element_types AS
    SELECT CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(x.objname AS sql_identifier) AS object_name,
           CAST(x.objtype AS character_data) AS object_type,
           CAST(x.objdtdid AS sql_identifier) AS array_type_identifier,
           CAST(
             CASE WHEN nbt.nspname = 'pg_catalog' THEN format_type(bt.oid, null)
                  ELSE 'USER-DEFINED' END AS character_data) AS data_type,

           CAST(null AS cardinal_number) AS character_maximum_length,
           CAST(null AS cardinal_number) AS character_octet_length,
           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,
           CAST(null AS sql_identifier) AS collation_catalog,
           CAST(null AS sql_identifier) AS collation_schema,
           CAST(null AS sql_identifier) AS collation_name,
           CAST(null AS cardinal_number) AS numeric_precision,
           CAST(null AS cardinal_number) AS numeric_precision_radix,
           CAST(null AS cardinal_number) AS numeric_scale,
           CAST(null AS cardinal_number) AS datetime_precision,
           CAST(null AS character_data) AS interval_type,
           CAST(null AS character_data) AS interval_precision,
           
           CAST(null AS character_data) AS domain_default, -- XXX maybe a bug in the standard

           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST('a' || x.objdtdid AS sql_identifier) AS dtd_identifier

    FROM pg_namespace n, pg_type at, pg_namespace nbt, pg_type bt,
         (
           /* columns */
           SELECT c.relnamespace, CAST(c.relname AS sql_identifier),
                  'TABLE'::text, a.attnum, a.atttypid
           FROM pg_class c, pg_attribute a
           WHERE c.oid = a.attrelid
                 AND c.relkind IN ('r', 'v')
                 AND attnum > 0 AND NOT attisdropped

           UNION ALL

           /* domains */
           SELECT t.typnamespace, CAST(t.typname AS sql_identifier),
                  'DOMAIN'::text, 1, t.typbasetype
           FROM pg_type t
           WHERE t.typtype = 'd'

           UNION ALL

           /* parameters */
           SELECT p.pronamespace, CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier),
                  'ROUTINE'::text, pos.n, p.proargtypes[pos.n-1]
           FROM pg_proc p, _pg_keypositions() AS pos(n)
           WHERE p.pronargs >= pos.n

           UNION ALL

           /* result types */
           SELECT p.pronamespace, CAST(p.proname || '_' || CAST(p.oid AS text) AS sql_identifier),
                  'ROUTINE'::text, 0, p.prorettype
           FROM pg_proc p

         ) AS x (objschema, objname, objtype, objdtdid, objtypeid)

    WHERE n.oid = x.objschema
          AND at.oid = x.objtypeid
          AND (at.typelem <> 0 AND at.typlen = -1)
          AND at.typelem = bt.oid
          AND nbt.oid = bt.typnamespace

          AND (n.nspname, x.objname, x.objtype, x.objdtdid) IN
              ( SELECT object_schema, object_name, object_type, dtd_identifier
                    FROM data_type_privileges );

GRANT SELECT ON element_types TO PUBLIC;
