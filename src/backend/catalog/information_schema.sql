/*
 * SQL Information Schema
 * as defined in ISO/IEC 9075-11:2016
 *
 * Copyright (c) 2003-2019, PostgreSQL Global Development Group
 *
 * src/backend/catalog/information_schema.sql
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
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
 * 5.1
 * INFORMATION_SCHEMA schema
 */

CREATE SCHEMA information_schema;
GRANT USAGE ON SCHEMA information_schema TO PUBLIC;
SET search_path TO information_schema;


/*
 * A few supporting functions first ...
 */

/* Expand any 1-D array into a set with integers 1..N */
CREATE FUNCTION _pg_expandarray(IN anyarray, OUT x anyelement, OUT n int)
    RETURNS SETOF RECORD
    LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE
    AS 'select $1[s], s - pg_catalog.array_lower($1,1) + 1
        from pg_catalog.generate_series(pg_catalog.array_lower($1,1),
                                        pg_catalog.array_upper($1,1),
                                        1) as g(s)';

CREATE FUNCTION _pg_keysequal(smallint[], smallint[]) RETURNS boolean
    LANGUAGE sql IMMUTABLE PARALLEL SAFE  -- intentionally not STRICT, to allow inlining
    AS 'select $1 operator(pg_catalog.<@) $2 and $2 operator(pg_catalog.<@) $1';

/* Given an index's OID and an underlying-table column number, return the
 * column's position in the index (NULL if not there) */
CREATE FUNCTION _pg_index_position(oid, smallint) RETURNS int
    LANGUAGE sql STRICT STABLE
    AS $$
SELECT (ss.a).n FROM
  (SELECT information_schema._pg_expandarray(indkey) AS a
   FROM pg_catalog.pg_index WHERE indexrelid = $1) ss
  WHERE (ss.a).x = $2;
$$;

CREATE FUNCTION _pg_truetypid(pg_attribute, pg_type) RETURNS oid
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT CASE WHEN $2.typtype = 'd' THEN $2.typbasetype ELSE $1.atttypid END$$;

CREATE FUNCTION _pg_truetypmod(pg_attribute, pg_type) RETURNS int4
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT CASE WHEN $2.typtype = 'd' THEN $2.typtypmod ELSE $1.atttypmod END$$;

-- these functions encapsulate knowledge about the encoding of typmod:

CREATE FUNCTION _pg_char_max_length(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $2 = -1 /* default typmod */
       THEN null
       WHEN $1 IN (1042, 1043) /* char, varchar */
       THEN $2 - 4
       WHEN $1 IN (1560, 1562) /* bit, varbit */
       THEN $2
       ELSE null
  END$$;

CREATE FUNCTION _pg_char_octet_length(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $1 IN (25, 1042, 1043) /* text, char, varchar */
       THEN CASE WHEN $2 = -1 /* default typmod */
                 THEN CAST(2^30 AS integer)
                 ELSE information_schema._pg_char_max_length($1, $2) *
                      pg_catalog.pg_encoding_max_length((SELECT encoding FROM pg_catalog.pg_database WHERE datname = pg_catalog.current_database()))
            END
       ELSE null
  END$$;

CREATE FUNCTION _pg_numeric_precision(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE $1
         WHEN 21 /*int2*/ THEN 16
         WHEN 23 /*int4*/ THEN 32
         WHEN 20 /*int8*/ THEN 64
         WHEN 1700 /*numeric*/ THEN
              CASE WHEN $2 = -1
                   THEN null
                   ELSE (($2 - 4) >> 16) & 65535
                   END
         WHEN 700 /*float4*/ THEN 24 /*FLT_MANT_DIG*/
         WHEN 701 /*float8*/ THEN 53 /*DBL_MANT_DIG*/
         ELSE null
  END$$;

CREATE FUNCTION _pg_numeric_precision_radix(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $1 IN (21, 23, 20, 700, 701) THEN 2
       WHEN $1 IN (1700) THEN 10
       ELSE null
  END$$;

CREATE FUNCTION _pg_numeric_scale(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $1 IN (21, 23, 20) THEN 0
       WHEN $1 IN (1700) THEN
            CASE WHEN $2 = -1
                 THEN null
                 ELSE ($2 - 4) & 65535
                 END
       ELSE null
  END$$;

CREATE FUNCTION _pg_datetime_precision(typid oid, typmod int4) RETURNS integer
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $1 IN (1082) /* date */
           THEN 0
       WHEN $1 IN (1083, 1114, 1184, 1266) /* time, timestamp, same + tz */
           THEN CASE WHEN $2 < 0 THEN 6 ELSE $2 END
       WHEN $1 IN (1186) /* interval */
           THEN CASE WHEN $2 < 0 OR $2 & 65535 = 65535 THEN 6 ELSE $2 & 65535 END
       ELSE null
  END$$;

CREATE FUNCTION _pg_interval_type(typid oid, mod int4) RETURNS text
    LANGUAGE sql
    IMMUTABLE
    PARALLEL SAFE
    RETURNS NULL ON NULL INPUT
    AS
$$SELECT
  CASE WHEN $1 IN (1186) /* interval */
           THEN pg_catalog.upper(substring(pg_catalog.format_type($1, $2) from 'interval[()0-9]* #"%#"' for '#'))
       ELSE null
  END$$;


-- 5.2 INFORMATION_SCHEMA_CATALOG_NAME view appears later.


/*
 * 5.3
 * CARDINAL_NUMBER domain
 */

CREATE DOMAIN cardinal_number AS integer
    CONSTRAINT cardinal_number_domain_check CHECK (value >= 0);


/*
 * 5.4
 * CHARACTER_DATA domain
 */

CREATE DOMAIN character_data AS character varying COLLATE "C";


/*
 * 5.5
 * SQL_IDENTIFIER domain
 */

CREATE DOMAIN sql_identifier AS name;


/*
 * 5.2
 * INFORMATION_SCHEMA_CATALOG_NAME view
 */

CREATE VIEW information_schema_catalog_name AS
    SELECT CAST(current_database() AS sql_identifier) AS catalog_name;

GRANT SELECT ON information_schema_catalog_name TO PUBLIC;


/*
 * 5.6
 * TIME_STAMP domain
 */

CREATE DOMAIN time_stamp AS timestamp(2) with time zone
    DEFAULT current_timestamp(2);

/*
 * 5.7
 * YES_OR_NO domain
 */

CREATE DOMAIN yes_or_no AS character varying(3) COLLATE "C"
    CONSTRAINT yes_or_no_check CHECK (value IN ('YES', 'NO'));


-- 5.8 ADMINISTRABLE_ROLE_AUTHORIZATIONS view appears later.


/*
 * 5.9
 * APPLICABLE_ROLES view
 */

CREATE VIEW applicable_roles AS
    SELECT CAST(a.rolname AS sql_identifier) AS grantee,
           CAST(b.rolname AS sql_identifier) AS role_name,
           CAST(CASE WHEN m.admin_option THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable
    FROM pg_auth_members m
         JOIN pg_authid a ON (m.member = a.oid)
         JOIN pg_authid b ON (m.roleid = b.oid)
    WHERE pg_has_role(a.oid, 'USAGE');

GRANT SELECT ON applicable_roles TO PUBLIC;


/*
 * 5.8
 * ADMINISTRABLE_ROLE_AUTHORIZATIONS view
 */

CREATE VIEW administrable_role_authorizations AS
    SELECT *
    FROM applicable_roles
    WHERE is_grantable = 'YES';

GRANT SELECT ON administrable_role_authorizations TO PUBLIC;


/*
 * 5.10
 * ASSERTIONS view
 */

-- feature not supported


/*
 * 5.11
 * ATTRIBUTES view
 */

CREATE VIEW attributes AS
    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nc.nspname AS sql_identifier) AS udt_schema,
           CAST(c.relname AS sql_identifier) AS udt_name,
           CAST(a.attname AS sql_identifier) AS attribute_name,
           CAST(a.attnum AS cardinal_number) AS ordinal_position,
           CAST(pg_get_expr(ad.adbin, ad.adrelid) AS character_data) AS attribute_default,
           CAST(CASE WHEN a.attnotnull OR (t.typtype = 'd' AND t.typnotnull) THEN 'NO' ELSE 'YES' END
             AS yes_or_no)
             AS is_nullable, -- This column was apparently removed between SQL:2003 and SQL:2008.

           CAST(
             CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
                  WHEN nt.nspname = 'pg_catalog' THEN format_type(a.atttypid, null)
                  ELSE 'USER-DEFINED' END
             AS character_data)
             AS data_type,

           CAST(
             _pg_char_max_length(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS character_maximum_length,

           CAST(
             _pg_char_octet_length(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS character_octet_length,

           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,

           CAST(CASE WHEN nco.nspname IS NOT NULL THEN current_database() END AS sql_identifier) AS collation_catalog,
           CAST(nco.nspname AS sql_identifier) AS collation_schema,
           CAST(co.collname AS sql_identifier) AS collation_name,

           CAST(
             _pg_numeric_precision(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_precision,

           CAST(
             _pg_numeric_precision_radix(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_precision_radix,

           CAST(
             _pg_numeric_scale(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_scale,

           CAST(
             _pg_datetime_precision(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS datetime_precision,

           CAST(
             _pg_interval_type(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS character_data)
             AS interval_type,
           CAST(null AS cardinal_number) AS interval_precision,

           CAST(current_database() AS sql_identifier) AS attribute_udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS attribute_udt_schema,
           CAST(t.typname AS sql_identifier) AS attribute_udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(a.attnum AS sql_identifier) AS dtd_identifier,
           CAST('NO' AS yes_or_no) AS is_derived_reference_attribute

    FROM (pg_attribute a LEFT JOIN pg_attrdef ad ON attrelid = adrelid AND attnum = adnum)
         JOIN (pg_class c JOIN pg_namespace nc ON (c.relnamespace = nc.oid)) ON a.attrelid = c.oid
         JOIN (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid)) ON a.atttypid = t.oid
         LEFT JOIN (pg_collation co JOIN pg_namespace nco ON (co.collnamespace = nco.oid))
           ON a.attcollation = co.oid AND (nco.nspname, co.collname) <> ('pg_catalog', 'default')

    WHERE a.attnum > 0 AND NOT a.attisdropped
          AND c.relkind IN ('c')
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_type_privilege(c.reltype, 'USAGE'));

GRANT SELECT ON attributes TO PUBLIC;


/*
 * 5.12
 * CHARACTER_SETS view
 */

CREATE VIEW character_sets AS
    SELECT CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(getdatabaseencoding() AS sql_identifier) AS character_set_name,
           CAST(CASE WHEN getdatabaseencoding() = 'UTF8' THEN 'UCS' ELSE getdatabaseencoding() END AS sql_identifier) AS character_repertoire,
           CAST(getdatabaseencoding() AS sql_identifier) AS form_of_use,
           CAST(current_database() AS sql_identifier) AS default_collate_catalog,
           CAST(nc.nspname AS sql_identifier) AS default_collate_schema,
           CAST(c.collname AS sql_identifier) AS default_collate_name
    FROM pg_database d
         LEFT JOIN (pg_collation c JOIN pg_namespace nc ON (c.collnamespace = nc.oid))
             ON (datcollate = collcollate AND datctype = collctype)
    WHERE d.datname = current_database()
    ORDER BY char_length(c.collname) DESC, c.collname ASC -- prefer full/canonical name
    LIMIT 1;

GRANT SELECT ON character_sets TO PUBLIC;


/*
 * 5.13
 * CHECK_CONSTRAINT_ROUTINE_USAGE view
 */

CREATE VIEW check_constraint_routine_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nc.nspname AS sql_identifier) AS constraint_schema,
           CAST(c.conname AS sql_identifier) AS constraint_name,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(np.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name
    FROM pg_namespace nc, pg_constraint c, pg_depend d, pg_proc p, pg_namespace np
    WHERE nc.oid = c.connamespace
      AND c.contype = 'c'
      AND c.oid = d.objid
      AND d.classid = 'pg_catalog.pg_constraint'::regclass
      AND d.refobjid = p.oid
      AND d.refclassid = 'pg_catalog.pg_proc'::regclass
      AND p.pronamespace = np.oid
      AND pg_has_role(p.proowner, 'USAGE');

GRANT SELECT ON check_constraint_routine_usage TO PUBLIC;


/*
 * 5.14
 * CHECK_CONSTRAINTS view
 */

CREATE VIEW check_constraints AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(rs.nspname AS sql_identifier) AS constraint_schema,
           CAST(con.conname AS sql_identifier) AS constraint_name,
           CAST(substring(pg_get_constraintdef(con.oid) from 7) AS character_data)
             AS check_clause
    FROM pg_constraint con
           LEFT OUTER JOIN pg_namespace rs ON (rs.oid = con.connamespace)
           LEFT OUTER JOIN pg_class c ON (c.oid = con.conrelid)
           LEFT OUTER JOIN pg_type t ON (t.oid = con.contypid)
    WHERE pg_has_role(coalesce(c.relowner, t.typowner), 'USAGE')
      AND con.contype = 'c'

    UNION
    -- not-null constraints

    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(n.nspname AS sql_identifier) AS constraint_schema,
           CAST(CAST(n.oid AS text) || '_' || CAST(r.oid AS text) || '_' || CAST(a.attnum AS text) || '_not_null' AS sql_identifier) AS constraint_name, -- XXX
           CAST(a.attname || ' IS NOT NULL' AS character_data)
             AS check_clause
    FROM pg_namespace n, pg_class r, pg_attribute a
    WHERE n.oid = r.relnamespace
      AND r.oid = a.attrelid
      AND a.attnum > 0
      AND NOT a.attisdropped
      AND a.attnotnull
      AND r.relkind IN ('r', 'p')
      AND pg_has_role(r.relowner, 'USAGE');

GRANT SELECT ON check_constraints TO PUBLIC;


/*
 * 5.15
 * COLLATIONS view
 */

CREATE VIEW collations AS
    SELECT CAST(current_database() AS sql_identifier) AS collation_catalog,
           CAST(nc.nspname AS sql_identifier) AS collation_schema,
           CAST(c.collname AS sql_identifier) AS collation_name,
           CAST('NO PAD' AS character_data) AS pad_attribute
    FROM pg_collation c, pg_namespace nc
    WHERE c.collnamespace = nc.oid
          AND collencoding IN (-1, (SELECT encoding FROM pg_database WHERE datname = current_database()));

GRANT SELECT ON collations TO PUBLIC;


/*
 * 5.16
 * COLLATION_CHARACTER_SET_APPLICABILITY view
 */

CREATE VIEW collation_character_set_applicability AS
    SELECT CAST(current_database() AS sql_identifier) AS collation_catalog,
           CAST(nc.nspname AS sql_identifier) AS collation_schema,
           CAST(c.collname AS sql_identifier) AS collation_name,
           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(getdatabaseencoding() AS sql_identifier) AS character_set_name
    FROM pg_collation c, pg_namespace nc
    WHERE c.collnamespace = nc.oid
          AND collencoding IN (-1, (SELECT encoding FROM pg_database WHERE datname = current_database()));

GRANT SELECT ON collation_character_set_applicability TO PUBLIC;


/*
 * 5.17
 * COLUMN_COLUMN_USAGE view
 */

CREATE VIEW column_column_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(n.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(ac.attname AS sql_identifier) AS column_name,
           CAST(ad.attname AS sql_identifier) AS dependent_column

    FROM pg_namespace n, pg_class c, pg_depend d,
         pg_attribute ac, pg_attribute ad

    WHERE n.oid = c.relnamespace
          AND c.oid = ac.attrelid
          AND c.oid = ad.attrelid
          AND d.classid = 'pg_catalog.pg_class'::regclass
          AND d.refclassid = 'pg_catalog.pg_class'::regclass
          AND d.objid = d.refobjid
          AND c.oid = d.objid
          AND d.objsubid = ad.attnum
          AND d.refobjsubid = ac.attnum
          AND ad.attgenerated <> ''
          AND pg_has_role(c.relowner, 'USAGE');

GRANT SELECT ON column_column_usage TO PUBLIC;


/*
 * 5.18
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
         pg_attribute a

    WHERE t.typnamespace = nt.oid
          AND c.relnamespace = nc.oid
          AND a.attrelid = c.oid
          AND a.atttypid = t.oid
          AND t.typtype = 'd'
          AND c.relkind IN ('r', 'v', 'f', 'p')
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND pg_has_role(t.typowner, 'USAGE');

GRANT SELECT ON column_domain_usage TO PUBLIC;


/*
 * 5.19
 * COLUMN_PRIVILEGES
 */

CREATE VIEW column_privileges AS
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(x.relname AS sql_identifier) AS table_name,
           CAST(x.attname AS sql_identifier) AS column_name,
           CAST(x.prtype AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(x.grantee, x.relowner, 'USAGE')
                  OR x.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
           SELECT pr_c.grantor,
                  pr_c.grantee,
                  attname,
                  relname,
                  relnamespace,
                  pr_c.prtype,
                  pr_c.grantable,
                  pr_c.relowner
           FROM (SELECT oid, relname, relnamespace, relowner, (aclexplode(coalesce(relacl, acldefault('r', relowner)))).*
                 FROM pg_class
                 WHERE relkind IN ('r', 'v', 'f', 'p')
                ) pr_c (oid, relname, relnamespace, relowner, grantor, grantee, prtype, grantable),
                pg_attribute a
           WHERE a.attrelid = pr_c.oid
                 AND a.attnum > 0
                 AND NOT a.attisdropped
           UNION
           SELECT pr_a.grantor,
                  pr_a.grantee,
                  attname,
                  relname,
                  relnamespace,
                  pr_a.prtype,
                  pr_a.grantable,
                  c.relowner
           FROM (SELECT attrelid, attname, (aclexplode(coalesce(attacl, acldefault('c', relowner)))).*
                 FROM pg_attribute a JOIN pg_class cc ON (a.attrelid = cc.oid)
                 WHERE attnum > 0
                       AND NOT attisdropped
                ) pr_a (attrelid, attname, grantor, grantee, prtype, grantable),
                pg_class c
           WHERE pr_a.attrelid = c.oid
                 AND relkind IN ('r', 'v', 'f', 'p')
         ) x,
         pg_namespace nc,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE x.relnamespace = nc.oid
          AND x.grantee = grantee.oid
          AND x.grantor = u_grantor.oid
          AND x.prtype IN ('INSERT', 'SELECT', 'UPDATE', 'REFERENCES')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC');

GRANT SELECT ON column_privileges TO PUBLIC;


/*
 * 5.20
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

    FROM pg_attribute a, pg_class c, pg_namespace nc,
         (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid))
           LEFT JOIN (pg_type bt JOIN pg_namespace nbt ON (bt.typnamespace = nbt.oid))
           ON (t.typtype = 'd' AND t.typbasetype = bt.oid)

    WHERE a.attrelid = c.oid
          AND a.atttypid = t.oid
          AND nc.oid = c.relnamespace
          AND a.attnum > 0 AND NOT a.attisdropped
          AND c.relkind in ('r', 'v', 'f', 'p')
          AND pg_has_role(coalesce(bt.typowner, t.typowner), 'USAGE');

GRANT SELECT ON column_udt_usage TO PUBLIC;


/*
 * 5.21
 * COLUMNS view
 */

CREATE VIEW columns AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST(a.attnum AS cardinal_number) AS ordinal_position,
           CAST(CASE WHEN a.attgenerated = '' THEN pg_get_expr(ad.adbin, ad.adrelid) END AS character_data) AS column_default,
           CAST(CASE WHEN a.attnotnull OR (t.typtype = 'd' AND t.typnotnull) THEN 'NO' ELSE 'YES' END
             AS yes_or_no)
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
             _pg_char_max_length(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS character_maximum_length,

           CAST(
             _pg_char_octet_length(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS character_octet_length,

           CAST(
             _pg_numeric_precision(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_precision,

           CAST(
             _pg_numeric_precision_radix(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_precision_radix,

           CAST(
             _pg_numeric_scale(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS numeric_scale,

           CAST(
             _pg_datetime_precision(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS cardinal_number)
             AS datetime_precision,

           CAST(
             _pg_interval_type(_pg_truetypid(a, t), _pg_truetypmod(a, t))
             AS character_data)
             AS interval_type,
           CAST(null AS cardinal_number) AS interval_precision,

           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,

           CAST(CASE WHEN nco.nspname IS NOT NULL THEN current_database() END AS sql_identifier) AS collation_catalog,
           CAST(nco.nspname AS sql_identifier) AS collation_schema,
           CAST(co.collname AS sql_identifier) AS collation_name,

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
           CAST('NO' AS yes_or_no) AS is_self_referencing,

           CAST(CASE WHEN a.attidentity IN ('a', 'd') THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_identity,
           CAST(CASE a.attidentity WHEN 'a' THEN 'ALWAYS' WHEN 'd' THEN 'BY DEFAULT' END AS character_data) AS identity_generation,
           CAST(seq.seqstart AS character_data) AS identity_start,
           CAST(seq.seqincrement AS character_data) AS identity_increment,
           CAST(seq.seqmax AS character_data) AS identity_maximum,
           CAST(seq.seqmin AS character_data) AS identity_minimum,
           CAST(CASE WHEN seq.seqcycle THEN 'YES' ELSE 'NO' END AS yes_or_no) AS identity_cycle,

           CAST(CASE WHEN a.attgenerated <> '' THEN 'ALWAYS' ELSE 'NEVER' END AS character_data) AS is_generated,
           CAST(CASE WHEN a.attgenerated <> '' THEN pg_get_expr(ad.adbin, ad.adrelid) END AS character_data) AS generation_expression,

           CAST(CASE WHEN c.relkind IN ('r', 'p') OR
                          (c.relkind IN ('v', 'f') AND
                           pg_column_is_updatable(c.oid, a.attnum, false))
                THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_updatable

    FROM (pg_attribute a LEFT JOIN pg_attrdef ad ON attrelid = adrelid AND attnum = adnum)
         JOIN (pg_class c JOIN pg_namespace nc ON (c.relnamespace = nc.oid)) ON a.attrelid = c.oid
         JOIN (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid)) ON a.atttypid = t.oid
         LEFT JOIN (pg_type bt JOIN pg_namespace nbt ON (bt.typnamespace = nbt.oid))
           ON (t.typtype = 'd' AND t.typbasetype = bt.oid)
         LEFT JOIN (pg_collation co JOIN pg_namespace nco ON (co.collnamespace = nco.oid))
           ON a.attcollation = co.oid AND (nco.nspname, co.collname) <> ('pg_catalog', 'default')
         LEFT JOIN (pg_depend dep JOIN pg_sequence seq ON (dep.classid = 'pg_class'::regclass AND dep.objid = seq.seqrelid AND dep.deptype = 'i'))
           ON (dep.refclassid = 'pg_class'::regclass AND dep.refobjid = c.oid AND dep.refobjsubid = a.attnum)

    WHERE (NOT pg_is_other_temp_schema(nc.oid))

          AND a.attnum > 0 AND NOT a.attisdropped
          AND c.relkind IN ('r', 'v', 'f', 'p')

          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_column_privilege(c.oid, a.attnum,
                                       'SELECT, INSERT, UPDATE, REFERENCES'));

GRANT SELECT ON columns TO PUBLIC;


/*
 * 5.22
 * CONSTRAINT_COLUMN_USAGE view
 */

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
            AND r.relkind IN ('r', 'p')
            AND NOT a.attisdropped

        UNION ALL

        /* unique/primary key/foreign key constraints */
        SELECT nr.nspname, r.relname, r.relowner, a.attname, nc.nspname, c.conname
          FROM pg_namespace nr, pg_class r, pg_attribute a, pg_namespace nc,
               pg_constraint c
          WHERE nr.oid = r.relnamespace
            AND r.oid = a.attrelid
            AND nc.oid = c.connamespace
            AND r.oid = CASE c.contype WHEN 'f' THEN c.confrelid ELSE c.conrelid END
            AND a.attnum = ANY (CASE c.contype WHEN 'f' THEN c.confkey ELSE c.conkey END)
            AND NOT a.attisdropped
            AND c.contype IN ('p', 'u', 'f')
            AND r.relkind IN ('r', 'p')

      ) AS x (tblschema, tblname, tblowner, colname, cstrschema, cstrname)

    WHERE pg_has_role(x.tblowner, 'USAGE');

GRANT SELECT ON constraint_column_usage TO PUBLIC;


/*
 * 5.23
 * CONSTRAINT_PERIOD_USAGE view
 */

-- feature not supported


/*
 * 5.24
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
         pg_class r, pg_namespace nr

    WHERE c.connamespace = nc.oid AND r.relnamespace = nr.oid
          AND ( (c.contype = 'f' AND c.confrelid = r.oid)
             OR (c.contype IN ('p', 'u') AND c.conrelid = r.oid) )
          AND r.relkind IN ('r', 'p')
          AND pg_has_role(r.relowner, 'USAGE');

GRANT SELECT ON constraint_table_usage TO PUBLIC;


-- 5.25 DATA_TYPE_PRIVILEGES view appears later.


/*
 * 5.26
 * DIRECT_SUPERTABLES view
 */

-- feature not supported


/*
 * 5.27
 * DIRECT_SUPERTYPES view
 */

-- feature not supported


/*
 * 5.28
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
             AS yes_or_no) AS is_deferrable,
           CAST(CASE WHEN condeferred THEN 'YES' ELSE 'NO' END
             AS yes_or_no) AS initially_deferred
    FROM pg_namespace rs, pg_namespace n, pg_constraint con, pg_type t
    WHERE rs.oid = con.connamespace
          AND n.oid = t.typnamespace
          AND t.oid = con.contypid
          AND (pg_has_role(t.typowner, 'USAGE')
               OR has_type_privilege(t.oid, 'USAGE'));

GRANT SELECT ON domain_constraints TO PUBLIC;


/*
 * DOMAIN_UDT_USAGE view
 * apparently removed in SQL:2003
 */

CREATE VIEW domain_udt_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,
           CAST(current_database() AS sql_identifier) AS domain_catalog,
           CAST(nt.nspname AS sql_identifier) AS domain_schema,
           CAST(t.typname AS sql_identifier) AS domain_name

    FROM pg_type t, pg_namespace nt,
         pg_type bt, pg_namespace nbt

    WHERE t.typnamespace = nt.oid
          AND t.typbasetype = bt.oid
          AND bt.typnamespace = nbt.oid
          AND t.typtype = 'd'
          AND pg_has_role(bt.typowner, 'USAGE');

GRANT SELECT ON domain_udt_usage TO PUBLIC;


/*
 * 5.29
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
             _pg_char_max_length(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS character_maximum_length,

           CAST(
             _pg_char_octet_length(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS character_octet_length,

           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,

           CAST(CASE WHEN nco.nspname IS NOT NULL THEN current_database() END AS sql_identifier) AS collation_catalog,
           CAST(nco.nspname AS sql_identifier) AS collation_schema,
           CAST(co.collname AS sql_identifier) AS collation_name,

           CAST(
             _pg_numeric_precision(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS numeric_precision,

           CAST(
             _pg_numeric_precision_radix(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS numeric_precision_radix,

           CAST(
             _pg_numeric_scale(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS numeric_scale,

           CAST(
             _pg_datetime_precision(t.typbasetype, t.typtypmod)
             AS cardinal_number)
             AS datetime_precision,

           CAST(
             _pg_interval_type(t.typbasetype, t.typtypmod)
             AS character_data)
             AS interval_type,
           CAST(null AS cardinal_number) AS interval_precision,

           CAST(t.typdefault AS character_data) AS domain_default,

           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(1 AS sql_identifier) AS dtd_identifier

    FROM (pg_type t JOIN pg_namespace nt ON t.typnamespace = nt.oid)
         JOIN (pg_type bt JOIN pg_namespace nbt ON bt.typnamespace = nbt.oid)
           ON (t.typbasetype = bt.oid AND t.typtype = 'd')
         LEFT JOIN (pg_collation co JOIN pg_namespace nco ON (co.collnamespace = nco.oid))
           ON t.typcollation = co.oid AND (nco.nspname, co.collname) <> ('pg_catalog', 'default')

    WHERE (pg_has_role(t.typowner, 'USAGE')
           OR has_type_privilege(t.oid, 'USAGE'));

GRANT SELECT ON domains TO PUBLIC;


-- 5.30 ELEMENT_TYPES view appears later.


/*
 * 5.31
 * ENABLED_ROLES view
 */

CREATE VIEW enabled_roles AS
    SELECT CAST(a.rolname AS sql_identifier) AS role_name
    FROM pg_authid a
    WHERE pg_has_role(a.oid, 'USAGE');

GRANT SELECT ON enabled_roles TO PUBLIC;


/*
 * 5.32
 * FIELDS view
 */

-- feature not supported


/*
 * 5.33
 * KEY_COLUMN_USAGE view
 */

CREATE VIEW key_column_usage AS
    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nc_nspname AS sql_identifier) AS constraint_schema,
           CAST(conname AS sql_identifier) AS constraint_name,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nr_nspname AS sql_identifier) AS table_schema,
           CAST(relname AS sql_identifier) AS table_name,
           CAST(a.attname AS sql_identifier) AS column_name,
           CAST((ss.x).n AS cardinal_number) AS ordinal_position,
           CAST(CASE WHEN contype = 'f' THEN
                       _pg_index_position(ss.conindid, ss.confkey[(ss.x).n])
                     ELSE NULL
                END AS cardinal_number)
             AS position_in_unique_constraint
    FROM pg_attribute a,
         (SELECT r.oid AS roid, r.relname, r.relowner,
                 nc.nspname AS nc_nspname, nr.nspname AS nr_nspname,
                 c.oid AS coid, c.conname, c.contype, c.conindid,
                 c.confkey, c.confrelid,
                 _pg_expandarray(c.conkey) AS x
          FROM pg_namespace nr, pg_class r, pg_namespace nc,
               pg_constraint c
          WHERE nr.oid = r.relnamespace
                AND r.oid = c.conrelid
                AND nc.oid = c.connamespace
                AND c.contype IN ('p', 'u', 'f')
                AND r.relkind IN ('r', 'p')
                AND (NOT pg_is_other_temp_schema(nr.oid)) ) AS ss
    WHERE ss.roid = a.attrelid
          AND a.attnum = (ss.x).x
          AND NOT a.attisdropped
          AND (pg_has_role(relowner, 'USAGE')
               OR has_column_privilege(roid, a.attnum,
                                       'SELECT, INSERT, UPDATE, REFERENCES'));

GRANT SELECT ON key_column_usage TO PUBLIC;


/*
 * 5.34
 * KEY_PERIOD_USAGE view
 */

-- feature not supported


/*
 * 5.35
 * METHOD_SPECIFICATION_PARAMETERS view
 */

-- feature not supported


/*
 * 5.36
 * METHOD_SPECIFICATIONS view
 */

-- feature not supported


/*
 * 5.37
 * PARAMETERS view
 */

CREATE VIEW parameters AS
    SELECT CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n_nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(proname, p_oid) AS sql_identifier) AS specific_name,
           CAST((ss.x).n AS cardinal_number) AS ordinal_position,
           CAST(
             CASE WHEN proargmodes IS NULL THEN 'IN'
                WHEN proargmodes[(ss.x).n] = 'i' THEN 'IN'
                WHEN proargmodes[(ss.x).n] = 'o' THEN 'OUT'
                WHEN proargmodes[(ss.x).n] = 'b' THEN 'INOUT'
                WHEN proargmodes[(ss.x).n] = 'v' THEN 'IN'
                WHEN proargmodes[(ss.x).n] = 't' THEN 'OUT'
             END AS character_data) AS parameter_mode,
           CAST('NO' AS yes_or_no) AS is_result,
           CAST('NO' AS yes_or_no) AS as_locator,
           CAST(NULLIF(proargnames[(ss.x).n], '') AS sql_identifier) AS parameter_name,
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
           CAST(null AS cardinal_number) AS interval_precision,
           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS udt_schema,
           CAST(t.typname AS sql_identifier) AS udt_name,
           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,
           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST((ss.x).n AS sql_identifier) AS dtd_identifier,
           CAST(
             CASE WHEN pg_has_role(proowner, 'USAGE')
                  THEN pg_get_function_arg_default(p_oid, (ss.x).n)
                  ELSE NULL END
             AS character_data) AS parameter_default

    FROM pg_type t, pg_namespace nt,
         (SELECT n.nspname AS n_nspname, p.proname, p.oid AS p_oid, p.proowner,
                 p.proargnames, p.proargmodes,
                 _pg_expandarray(coalesce(p.proallargtypes, p.proargtypes::oid[])) AS x
          FROM pg_namespace n, pg_proc p
          WHERE n.oid = p.pronamespace
                AND (pg_has_role(p.proowner, 'USAGE') OR
                     has_function_privilege(p.oid, 'EXECUTE'))) AS ss
    WHERE t.oid = (ss.x).x AND t.typnamespace = nt.oid;

GRANT SELECT ON parameters TO PUBLIC;


/*
 * 5.38
 * PERIODS view
 */

-- feature not supported


/*
 * 5.39
 * PRIVATE_PARAMETERS view
 */

-- feature not supported


/*
 * 5.40
 * REFERENCED_TYPES view
 */

-- feature not supported


/*
 * 5.41
 * REFERENTIAL_CONSTRAINTS view
 */

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
                                    WHEN 's' THEN 'NONE' END
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

    FROM (pg_namespace ncon
          INNER JOIN pg_constraint con ON ncon.oid = con.connamespace
          INNER JOIN pg_class c ON con.conrelid = c.oid AND con.contype = 'f')
         LEFT JOIN pg_depend d1  -- find constraint's dependency on an index
          ON d1.objid = con.oid AND d1.classid = 'pg_constraint'::regclass
             AND d1.refclassid = 'pg_class'::regclass AND d1.refobjsubid = 0
         LEFT JOIN pg_depend d2  -- find pkey/unique constraint for that index
          ON d2.refclassid = 'pg_constraint'::regclass
             AND d2.classid = 'pg_class'::regclass
             AND d2.objid = d1.refobjid AND d2.objsubid = 0
             AND d2.deptype = 'i'
         LEFT JOIN pg_constraint pkc ON pkc.oid = d2.refobjid
            AND pkc.contype IN ('p', 'u')
            AND pkc.conrelid = con.confrelid
         LEFT JOIN pg_namespace npkc ON pkc.connamespace = npkc.oid

    WHERE pg_has_role(c.relowner, 'USAGE')
          -- SELECT privilege omitted, per SQL standard
          OR has_table_privilege(c.oid, 'INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
          OR has_any_column_privilege(c.oid, 'INSERT, UPDATE, REFERENCES') ;

GRANT SELECT ON referential_constraints TO PUBLIC;


/*
 * 5.42
 * ROLE_COLUMN_GRANTS view
 */

CREATE VIEW role_column_grants AS
    SELECT grantor,
           grantee,
           table_catalog,
           table_schema,
           table_name,
           column_name,
           privilege_type,
           is_grantable
    FROM column_privileges
    WHERE grantor IN (SELECT role_name FROM enabled_roles)
          OR grantee IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_column_grants TO PUBLIC;


-- 5.43 ROLE_ROUTINE_GRANTS view is based on 5.50 ROUTINE_PRIVILEGES and is defined there instead.


-- 5.44 ROLE_TABLE_GRANTS view is based on 5.63 TABLE_PRIVILEGES and is defined there instead.


/*
 * 5.45
 * ROLE_TABLE_METHOD_GRANTS view
 */

-- feature not supported



-- 5.46 ROLE_USAGE_GRANTS view is based on 5.75 USAGE_PRIVILEGES and is defined there instead.


-- 5.47 ROLE_UDT_GRANTS view is based on 5.74 UDT_PRIVILEGES and is defined there instead.


/*
 * 5.48
 * ROUTINE_COLUMN_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.49
 * ROUTINE_PERIOD_USAGE view
 */

-- feature not supported


/*
 * 5.50
 * ROUTINE_PRIVILEGES view
 */

CREATE VIEW routine_privileges AS
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name,
           CAST(current_database() AS sql_identifier) AS routine_catalog,
           CAST(n.nspname AS sql_identifier) AS routine_schema,
           CAST(p.proname AS sql_identifier) AS routine_name,
           CAST('EXECUTE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, p.proowner, 'USAGE')
                  OR p.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT oid, proname, proowner, pronamespace, (aclexplode(coalesce(proacl, acldefault('f', proowner)))).* FROM pg_proc
         ) p (oid, proname, proowner, pronamespace, grantor, grantee, prtype, grantable),
         pg_namespace n,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE p.pronamespace = n.oid
          AND grantee.oid = p.grantee
          AND u_grantor.oid = p.grantor
          AND p.prtype IN ('EXECUTE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC');

GRANT SELECT ON routine_privileges TO PUBLIC;


/*
 * 5.42
 * ROLE_ROUTINE_GRANTS view
 */

CREATE VIEW role_routine_grants AS
    SELECT grantor,
           grantee,
           specific_catalog,
           specific_schema,
           specific_name,
           routine_catalog,
           routine_schema,
           routine_name,
           privilege_type,
           is_grantable
    FROM routine_privileges
    WHERE grantor IN (SELECT role_name FROM enabled_roles)
          OR grantee IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_routine_grants TO PUBLIC;


/*
 * 5.51
 * ROUTINE_ROUTINE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.52
 * ROUTINE_SEQUENCE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.53
 * ROUTINE_TABLE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.54
 * ROUTINES view
 */

CREATE VIEW routines AS
    SELECT CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(n.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name,
           CAST(current_database() AS sql_identifier) AS routine_catalog,
           CAST(n.nspname AS sql_identifier) AS routine_schema,
           CAST(p.proname AS sql_identifier) AS routine_name,
           CAST(CASE p.prokind WHEN 'f' THEN 'FUNCTION' WHEN 'p' THEN 'PROCEDURE' END
             AS character_data) AS routine_type,
           CAST(null AS sql_identifier) AS module_catalog,
           CAST(null AS sql_identifier) AS module_schema,
           CAST(null AS sql_identifier) AS module_name,
           CAST(null AS sql_identifier) AS udt_catalog,
           CAST(null AS sql_identifier) AS udt_schema,
           CAST(null AS sql_identifier) AS udt_name,

           CAST(
             CASE WHEN p.prokind = 'p' THEN NULL
                  WHEN t.typelem <> 0 AND t.typlen = -1 THEN 'ARRAY'
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
           CAST(null AS cardinal_number) AS interval_precision,
           CAST(CASE WHEN nt.nspname IS NOT NULL THEN current_database() END AS sql_identifier) AS type_udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS type_udt_schema,
           CAST(t.typname AS sql_identifier) AS type_udt_name,
           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,
           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST(CASE WHEN p.prokind <> 'p' THEN 0 END AS sql_identifier) AS dtd_identifier,

           CAST(CASE WHEN l.lanname = 'sql' THEN 'SQL' ELSE 'EXTERNAL' END AS character_data)
             AS routine_body,
           CAST(
             CASE WHEN pg_has_role(p.proowner, 'USAGE') THEN p.prosrc ELSE null END
             AS character_data) AS routine_definition,
           CAST(
             CASE WHEN l.lanname = 'c' THEN p.prosrc ELSE null END
             AS character_data) AS external_name,
           CAST(upper(l.lanname) AS character_data) AS external_language,

           CAST('GENERAL' AS character_data) AS parameter_style,
           CAST(CASE WHEN p.provolatile = 'i' THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_deterministic,
           CAST('MODIFIES' AS character_data) AS sql_data_access,
           CAST(CASE WHEN p.prokind <> 'p' THEN
             CASE WHEN p.proisstrict THEN 'YES' ELSE 'NO' END END AS yes_or_no) AS is_null_call,
           CAST(null AS character_data) AS sql_path,
           CAST('YES' AS yes_or_no) AS schema_level_routine,
           CAST(0 AS cardinal_number) AS max_dynamic_result_sets,
           CAST(null AS yes_or_no) AS is_user_defined_cast,
           CAST(null AS yes_or_no) AS is_implicitly_invocable,
           CAST(CASE WHEN p.prosecdef THEN 'DEFINER' ELSE 'INVOKER' END AS character_data) AS security_type,
           CAST(null AS sql_identifier) AS to_sql_specific_catalog,
           CAST(null AS sql_identifier) AS to_sql_specific_schema,
           CAST(null AS sql_identifier) AS to_sql_specific_name,
           CAST('NO' AS yes_or_no) AS as_locator,
           CAST(null AS time_stamp) AS created,
           CAST(null AS time_stamp) AS last_altered,
           CAST(null AS yes_or_no) AS new_savepoint_level,
           CAST('NO' AS yes_or_no) AS is_udt_dependent,

           CAST(null AS character_data) AS result_cast_from_data_type,
           CAST(null AS yes_or_no) AS result_cast_as_locator,
           CAST(null AS cardinal_number) AS result_cast_char_max_length,
           CAST(null AS cardinal_number) AS result_cast_char_octet_length,
           CAST(null AS sql_identifier) AS result_cast_char_set_catalog,
           CAST(null AS sql_identifier) AS result_cast_char_set_schema,
           CAST(null AS sql_identifier) AS result_cast_char_set_name,
           CAST(null AS sql_identifier) AS result_cast_collation_catalog,
           CAST(null AS sql_identifier) AS result_cast_collation_schema,
           CAST(null AS sql_identifier) AS result_cast_collation_name,
           CAST(null AS cardinal_number) AS result_cast_numeric_precision,
           CAST(null AS cardinal_number) AS result_cast_numeric_precision_radix,
           CAST(null AS cardinal_number) AS result_cast_numeric_scale,
           CAST(null AS cardinal_number) AS result_cast_datetime_precision,
           CAST(null AS character_data) AS result_cast_interval_type,
           CAST(null AS cardinal_number) AS result_cast_interval_precision,
           CAST(null AS sql_identifier) AS result_cast_type_udt_catalog,
           CAST(null AS sql_identifier) AS result_cast_type_udt_schema,
           CAST(null AS sql_identifier) AS result_cast_type_udt_name,
           CAST(null AS sql_identifier) AS result_cast_scope_catalog,
           CAST(null AS sql_identifier) AS result_cast_scope_schema,
           CAST(null AS sql_identifier) AS result_cast_scope_name,
           CAST(null AS cardinal_number) AS result_cast_maximum_cardinality,
           CAST(null AS sql_identifier) AS result_cast_dtd_identifier

    FROM (pg_namespace n
          JOIN pg_proc p ON n.oid = p.pronamespace
          JOIN pg_language l ON p.prolang = l.oid)
         LEFT JOIN
         (pg_type t JOIN pg_namespace nt ON t.typnamespace = nt.oid)
         ON p.prorettype = t.oid AND p.prokind <> 'p'

    WHERE (pg_has_role(p.proowner, 'USAGE')
           OR has_function_privilege(p.oid, 'EXECUTE'));

GRANT SELECT ON routines TO PUBLIC;


/*
 * 5.55
 * SCHEMATA view
 */

CREATE VIEW schemata AS
    SELECT CAST(current_database() AS sql_identifier) AS catalog_name,
           CAST(n.nspname AS sql_identifier) AS schema_name,
           CAST(u.rolname AS sql_identifier) AS schema_owner,
           CAST(null AS sql_identifier) AS default_character_set_catalog,
           CAST(null AS sql_identifier) AS default_character_set_schema,
           CAST(null AS sql_identifier) AS default_character_set_name,
           CAST(null AS character_data) AS sql_path
    FROM pg_namespace n, pg_authid u
    WHERE n.nspowner = u.oid
          AND (pg_has_role(n.nspowner, 'USAGE')
               OR has_schema_privilege(n.oid, 'CREATE, USAGE'));

GRANT SELECT ON schemata TO PUBLIC;


/*
 * 5.56
 * SEQUENCES view
 */

CREATE VIEW sequences AS
    SELECT CAST(current_database() AS sql_identifier) AS sequence_catalog,
           CAST(nc.nspname AS sql_identifier) AS sequence_schema,
           CAST(c.relname AS sql_identifier) AS sequence_name,
           CAST(format_type(s.seqtypid, null) AS character_data) AS data_type,
           CAST(_pg_numeric_precision(s.seqtypid, -1) AS cardinal_number) AS numeric_precision,
           CAST(2 AS cardinal_number) AS numeric_precision_radix,
           CAST(0 AS cardinal_number) AS numeric_scale,
           CAST(s.seqstart AS character_data) AS start_value,
           CAST(s.seqmin AS character_data) AS minimum_value,
           CAST(s.seqmax AS character_data) AS maximum_value,
           CAST(s.seqincrement AS character_data) AS increment,
           CAST(CASE WHEN s.seqcycle THEN 'YES' ELSE 'NO' END AS yes_or_no) AS cycle_option
    FROM pg_namespace nc, pg_class c, pg_sequence s
    WHERE c.relnamespace = nc.oid
          AND c.relkind = 'S'
          AND NOT EXISTS (SELECT 1 FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = c.oid AND deptype = 'i')
          AND (NOT pg_is_other_temp_schema(nc.oid))
          AND c.oid = s.seqrelid
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_sequence_privilege(c.oid, 'SELECT, UPDATE, USAGE') );

GRANT SELECT ON sequences TO PUBLIC;


/*
 * 5.57
 * SQL_FEATURES table
 */

CREATE TABLE sql_features (
    feature_id          character_data,
    feature_name        character_data,
    sub_feature_id      character_data,
    sub_feature_name    character_data,
    is_supported        yes_or_no,
    is_verified_by      character_data,
    comments            character_data
);

-- Will be filled with external data by initdb.

GRANT SELECT ON sql_features TO PUBLIC;


/*
 * 5.58
 * SQL_IMPLEMENTATION_INFO table
 */

-- Note: Implementation information items are defined in ISO/IEC 9075-3:2008,
-- clause 9.1.

CREATE TABLE sql_implementation_info (
    implementation_info_id      character_data,
    implementation_info_name    character_data,
    integer_value               cardinal_number,
    character_value             character_data,
    comments                    character_data
);

INSERT INTO sql_implementation_info VALUES ('10003', 'CATALOG NAME', NULL, 'Y', NULL);
INSERT INTO sql_implementation_info VALUES ('10004', 'COLLATING SEQUENCE', NULL, (SELECT default_collate_name FROM character_sets), NULL);
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
 * SQL_LANGUAGES table
 * apparently removed in SQL:2008
 */

CREATE TABLE sql_languages (
    sql_language_source         character_data,
    sql_language_year           character_data,
    sql_language_conformance    character_data,
    sql_language_integrity      character_data,
    sql_language_implementation character_data,
    sql_language_binding_style  character_data,
    sql_language_programming_language character_data
);

INSERT INTO sql_languages VALUES ('ISO 9075', '1999', 'CORE', NULL, NULL, 'DIRECT', NULL);
INSERT INTO sql_languages VALUES ('ISO 9075', '1999', 'CORE', NULL, NULL, 'EMBEDDED', 'C');
INSERT INTO sql_languages VALUES ('ISO 9075', '2003', 'CORE', NULL, NULL, 'DIRECT', NULL);
INSERT INTO sql_languages VALUES ('ISO 9075', '2003', 'CORE', NULL, NULL, 'EMBEDDED', 'C');

GRANT SELECT ON sql_languages TO PUBLIC;


/*
 * SQL_PACKAGES table
 * removed in SQL:2011
 */

CREATE TABLE sql_packages (
    feature_id      character_data,
    feature_name    character_data,
    is_supported    yes_or_no,
    is_verified_by  character_data,
    comments        character_data
);

INSERT INTO sql_packages VALUES ('PKG000', 'Core', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG001', 'Enhanced datetime facilities', 'YES', NULL, '');
INSERT INTO sql_packages VALUES ('PKG002', 'Enhanced integrity management', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG003', 'OLAP facilities', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG004', 'PSM', 'NO', NULL, 'PL/pgSQL is similar.');
INSERT INTO sql_packages VALUES ('PKG005', 'CLI', 'NO', NULL, 'ODBC is similar.');
INSERT INTO sql_packages VALUES ('PKG006', 'Basic object support', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG007', 'Enhanced object support', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG008', 'Active database', 'NO', NULL, '');
INSERT INTO sql_packages VALUES ('PKG010', 'OLAP', 'NO', NULL, 'NO');

GRANT SELECT ON sql_packages TO PUBLIC;


/*
 * 5.59
 * SQL_PARTS table
 */

CREATE TABLE sql_parts (
    feature_id      character_data,
    feature_name    character_data,
    is_supported    yes_or_no,
    is_verified_by  character_data,
    comments        character_data
);

INSERT INTO sql_parts VALUES ('1', 'Framework (SQL/Framework)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('2', 'Foundation (SQL/Foundation)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('3', 'Call-Level Interface (SQL/CLI)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('4', 'Persistent Stored Modules (SQL/PSM)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('9', 'Management of External Data (SQL/MED)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('10', 'Object Language Bindings (SQL/OLB)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('11', 'Information and Definition Schema (SQL/Schemata)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('13', 'Routines and Types Using the Java Programming Language (SQL/JRT)', 'NO', NULL, '');
INSERT INTO sql_parts VALUES ('14', 'XML-Related Specifications (SQL/XML)', 'NO', NULL, '');


/*
 * 5.60
 * SQL_SIZING table
 */

-- Note: Sizing items are defined in ISO/IEC 9075-3:2008, clause 9.2.

CREATE TABLE sql_sizing (
    sizing_id       cardinal_number,
    sizing_name     character_data,
    supported_value cardinal_number,
    comments        character_data
);

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
 * SQL_SIZING_PROFILES table
 * removed in SQL:2011
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
);

GRANT SELECT ON sql_sizing_profiles TO PUBLIC;


/*
 * 5.61
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
           CAST(CASE WHEN c.condeferrable THEN 'YES' ELSE 'NO' END AS yes_or_no)
             AS is_deferrable,
           CAST(CASE WHEN c.condeferred THEN 'YES' ELSE 'NO' END AS yes_or_no)
             AS initially_deferred,
           CAST('YES' AS yes_or_no) AS enforced

    FROM pg_namespace nc,
         pg_namespace nr,
         pg_constraint c,
         pg_class r

    WHERE nc.oid = c.connamespace AND nr.oid = r.relnamespace
          AND c.conrelid = r.oid
          AND c.contype NOT IN ('t', 'x')  -- ignore nonstandard constraints
          AND r.relkind IN ('r', 'p')
          AND (NOT pg_is_other_temp_schema(nr.oid))
          AND (pg_has_role(r.relowner, 'USAGE')
               -- SELECT privilege omitted, per SQL standard
               OR has_table_privilege(r.oid, 'INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(r.oid, 'INSERT, UPDATE, REFERENCES') )

    UNION ALL

    -- not-null constraints

    SELECT CAST(current_database() AS sql_identifier) AS constraint_catalog,
           CAST(nr.nspname AS sql_identifier) AS constraint_schema,
           CAST(CAST(nr.oid AS text) || '_' || CAST(r.oid AS text) || '_' || CAST(a.attnum AS text) || '_not_null' AS sql_identifier) AS constraint_name, -- XXX
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nr.nspname AS sql_identifier) AS table_schema,
           CAST(r.relname AS sql_identifier) AS table_name,
           CAST('CHECK' AS character_data) AS constraint_type,
           CAST('NO' AS yes_or_no) AS is_deferrable,
           CAST('NO' AS yes_or_no) AS initially_deferred,
           CAST('YES' AS yes_or_no) AS enforced

    FROM pg_namespace nr,
         pg_class r,
         pg_attribute a

    WHERE nr.oid = r.relnamespace
          AND r.oid = a.attrelid
          AND a.attnotnull
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND r.relkind IN ('r', 'p')
          AND (NOT pg_is_other_temp_schema(nr.oid))
          AND (pg_has_role(r.relowner, 'USAGE')
               -- SELECT privilege omitted, per SQL standard
               OR has_table_privilege(r.oid, 'INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(r.oid, 'INSERT, UPDATE, REFERENCES') );

GRANT SELECT ON table_constraints TO PUBLIC;


/*
 * 5.62
 * TABLE_METHOD_PRIVILEGES view
 */

-- feature not supported


/*
 * 5.63
 * TABLE_PRIVILEGES view
 */

CREATE VIEW table_privileges AS
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(c.prtype AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, c.relowner, 'USAGE')
                  OR c.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable,
           CAST(CASE WHEN c.prtype = 'SELECT' THEN 'YES' ELSE 'NO' END AS yes_or_no) AS with_hierarchy

    FROM (
            SELECT oid, relname, relnamespace, relkind, relowner, (aclexplode(coalesce(relacl, acldefault('r', relowner)))).* FROM pg_class
         ) AS c (oid, relname, relnamespace, relkind, relowner, grantor, grantee, prtype, grantable),
         pg_namespace nc,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE c.relnamespace = nc.oid
          AND c.relkind IN ('r', 'v', 'f', 'p')
          AND c.grantee = grantee.oid
          AND c.grantor = u_grantor.oid
          AND c.prtype IN ('INSERT', 'SELECT', 'UPDATE', 'DELETE', 'TRUNCATE', 'REFERENCES', 'TRIGGER')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC');

GRANT SELECT ON table_privileges TO PUBLIC;


/*
 * 5.43
 * ROLE_TABLE_GRANTS view
 */

CREATE VIEW role_table_grants AS
    SELECT grantor,
           grantee,
           table_catalog,
           table_schema,
           table_name,
           privilege_type,
           is_grantable,
           with_hierarchy
    FROM table_privileges
    WHERE grantor IN (SELECT role_name FROM enabled_roles)
          OR grantee IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_table_grants TO PUBLIC;


/*
 * 5.63
 * TABLES view
 */

CREATE VIEW tables AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,

           CAST(
             CASE WHEN nc.oid = pg_my_temp_schema() THEN 'LOCAL TEMPORARY'
                  WHEN c.relkind IN ('r', 'p') THEN 'BASE TABLE'
                  WHEN c.relkind = 'v' THEN 'VIEW'
                  WHEN c.relkind = 'f' THEN 'FOREIGN'
                  ELSE null END
             AS character_data) AS table_type,

           CAST(null AS sql_identifier) AS self_referencing_column_name,
           CAST(null AS character_data) AS reference_generation,

           CAST(CASE WHEN t.typname IS NOT NULL THEN current_database() ELSE null END AS sql_identifier) AS user_defined_type_catalog,
           CAST(nt.nspname AS sql_identifier) AS user_defined_type_schema,
           CAST(t.typname AS sql_identifier) AS user_defined_type_name,

           CAST(CASE WHEN c.relkind IN ('r', 'p') OR
                          (c.relkind IN ('v', 'f') AND
                           -- 1 << CMD_INSERT
                           pg_relation_is_updatable(c.oid, false) & 8 = 8)
                THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_insertable_into,

           CAST(CASE WHEN t.typname IS NOT NULL THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_typed,
           CAST(null AS character_data) AS commit_action

    FROM pg_namespace nc JOIN pg_class c ON (nc.oid = c.relnamespace)
           LEFT JOIN (pg_type t JOIN pg_namespace nt ON (t.typnamespace = nt.oid)) ON (c.reloftype = t.oid)

    WHERE c.relkind IN ('r', 'v', 'f', 'p')
          AND (NOT pg_is_other_temp_schema(nc.oid))
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(c.oid, 'SELECT, INSERT, UPDATE, REFERENCES') );

GRANT SELECT ON tables TO PUBLIC;


/*
 * 5.65
 * TRANSFORMS view
 */

CREATE VIEW transforms AS
    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS udt_schema,
           CAST(t.typname AS sql_identifier) AS udt_name,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(np.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name,
           CAST(l.lanname AS sql_identifier) AS group_name,
           CAST('FROM SQL' AS character_data) AS transform_type
    FROM pg_type t JOIN pg_transform x ON t.oid = x.trftype
         JOIN pg_language l ON x.trflang = l.oid
         JOIN pg_proc p ON x.trffromsql = p.oid
         JOIN pg_namespace nt ON t.typnamespace = nt.oid
         JOIN pg_namespace np ON p.pronamespace = np.oid

  UNION

    SELECT CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nt.nspname AS sql_identifier) AS udt_schema,
           CAST(t.typname AS sql_identifier) AS udt_name,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(np.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name,
           CAST(l.lanname AS sql_identifier) AS group_name,
           CAST('TO SQL' AS character_data) AS transform_type
    FROM pg_type t JOIN pg_transform x ON t.oid = x.trftype
         JOIN pg_language l ON x.trflang = l.oid
         JOIN pg_proc p ON x.trftosql = p.oid
         JOIN pg_namespace nt ON t.typnamespace = nt.oid
         JOIN pg_namespace np ON p.pronamespace = np.oid

  ORDER BY udt_catalog, udt_schema, udt_name, group_name, transform_type  -- some sensible grouping for interactive use
;


/*
 * 5.66
 * TRANSLATIONS view
 */

-- feature not supported


/*
 * 5.67
 * TRIGGERED_UPDATE_COLUMNS view
 */

CREATE VIEW triggered_update_columns AS
    SELECT CAST(current_database() AS sql_identifier) AS trigger_catalog,
           CAST(n.nspname AS sql_identifier) AS trigger_schema,
           CAST(t.tgname AS sql_identifier) AS trigger_name,
           CAST(current_database() AS sql_identifier) AS event_object_catalog,
           CAST(n.nspname AS sql_identifier) AS event_object_schema,
           CAST(c.relname AS sql_identifier) AS event_object_table,
           CAST(a.attname AS sql_identifier) AS event_object_column

    FROM pg_namespace n, pg_class c, pg_trigger t,
         (SELECT tgoid, (ta0.tgat).x AS tgattnum, (ta0.tgat).n AS tgattpos
          FROM (SELECT oid AS tgoid, information_schema._pg_expandarray(tgattr) AS tgat FROM pg_trigger) AS ta0) AS ta,
         pg_attribute a

    WHERE n.oid = c.relnamespace
          AND c.oid = t.tgrelid
          AND t.oid = ta.tgoid
          AND (a.attrelid, a.attnum) = (t.tgrelid, ta.tgattnum)
          AND NOT t.tgisinternal
          AND (NOT pg_is_other_temp_schema(n.oid))
          AND (pg_has_role(c.relowner, 'USAGE')
               -- SELECT privilege omitted, per SQL standard
               OR has_column_privilege(c.oid, a.attnum, 'INSERT, UPDATE, REFERENCES') );

GRANT SELECT ON triggered_update_columns TO PUBLIC;


/*
 * 5.68
 * TRIGGER_COLUMN_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.69
 * TRIGGER_PERIOD_USAGE view
 */

-- feature not supported


/*
 * 5.70
 * TRIGGER_ROUTINE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.71
 * TRIGGER_SEQUENCE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.72
 * TRIGGER_TABLE_USAGE view
 */

-- not tracked by PostgreSQL


/*
 * 5.73
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
           CAST(
             -- To determine action order, partition by schema, table,
             -- event_manipulation (INSERT/DELETE/UPDATE), ROW/STATEMENT (1),
             -- BEFORE/AFTER (66), then order by trigger name.  It's preferable
             -- to partition by view output columns, so that query constraints
             -- can be pushed down below the window function.
             rank() OVER (PARTITION BY CAST(n.nspname AS sql_identifier),
                                       CAST(c.relname AS sql_identifier),
                                       em.num,
                                       t.tgtype & 1,
                                       t.tgtype & 66
                                       ORDER BY t.tgname)
             AS cardinal_number) AS action_order,
           CAST(
             CASE WHEN pg_has_role(c.relowner, 'USAGE')
               THEN (regexp_match(pg_get_triggerdef(t.oid), E'.{35,} WHEN \\((.+)\\) EXECUTE FUNCTION'))[1]
               ELSE null END
             AS character_data) AS action_condition,
           CAST(
             substring(pg_get_triggerdef(t.oid) from
                       position('EXECUTE FUNCTION' in substring(pg_get_triggerdef(t.oid) from 48)) + 47)
             AS character_data) AS action_statement,
           CAST(
             -- hard-wired reference to TRIGGER_TYPE_ROW
             CASE t.tgtype & 1 WHEN 1 THEN 'ROW' ELSE 'STATEMENT' END
             AS character_data) AS action_orientation,
           CAST(
             -- hard-wired refs to TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSTEAD
             CASE t.tgtype & 66 WHEN 2 THEN 'BEFORE' WHEN 64 THEN 'INSTEAD OF' ELSE 'AFTER' END
             AS character_data) AS action_timing,
           CAST(tgoldtable AS sql_identifier) AS action_reference_old_table,
           CAST(tgnewtable AS sql_identifier) AS action_reference_new_table,
           CAST(null AS sql_identifier) AS action_reference_old_row,
           CAST(null AS sql_identifier) AS action_reference_new_row,
           CAST(null AS time_stamp) AS created

    FROM pg_namespace n, pg_class c, pg_trigger t,
         -- hard-wired refs to TRIGGER_TYPE_INSERT, TRIGGER_TYPE_DELETE,
         -- TRIGGER_TYPE_UPDATE; we intentionally omit TRIGGER_TYPE_TRUNCATE
         (VALUES (4, 'INSERT'),
                 (8, 'DELETE'),
                 (16, 'UPDATE')) AS em (num, text)

    WHERE n.oid = c.relnamespace
          AND c.oid = t.tgrelid
          AND t.tgtype & em.num <> 0
          AND NOT t.tgisinternal
          AND (NOT pg_is_other_temp_schema(n.oid))
          AND (pg_has_role(c.relowner, 'USAGE')
               -- SELECT privilege omitted, per SQL standard
               OR has_table_privilege(c.oid, 'INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(c.oid, 'INSERT, UPDATE, REFERENCES') );

GRANT SELECT ON triggers TO PUBLIC;


/*
 * 5.74
 * UDT_PRIVILEGES view
 */

CREATE VIEW udt_privileges AS
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(n.nspname AS sql_identifier) AS udt_schema,
           CAST(t.typname AS sql_identifier) AS udt_name,
           CAST('TYPE USAGE' AS character_data) AS privilege_type, -- sic
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, t.typowner, 'USAGE')
                  OR t.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT oid, typname, typnamespace, typtype, typowner, (aclexplode(coalesce(typacl, acldefault('T', typowner)))).* FROM pg_type
         ) AS t (oid, typname, typnamespace, typtype, typowner, grantor, grantee, prtype, grantable),
         pg_namespace n,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE t.typnamespace = n.oid
          AND t.typtype = 'c'
          AND t.grantee = grantee.oid
          AND t.grantor = u_grantor.oid
          AND t.prtype IN ('USAGE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC');

GRANT SELECT ON udt_privileges TO PUBLIC;


/*
 * 5.46
 * ROLE_UDT_GRANTS view
 */

CREATE VIEW role_udt_grants AS
    SELECT grantor,
           grantee,
           udt_catalog,
           udt_schema,
           udt_name,
           privilege_type,
           is_grantable
    FROM udt_privileges
    WHERE grantor IN (SELECT role_name FROM enabled_roles)
          OR grantee IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_udt_grants TO PUBLIC;


/*
 * 5.75
 * USAGE_PRIVILEGES view
 */

CREATE VIEW usage_privileges AS

    /* collations */
    -- Collations have no real privileges, so we represent all collations with implicit usage privilege here.
    SELECT CAST(u.rolname AS sql_identifier) AS grantor,
           CAST('PUBLIC' AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(c.collname AS sql_identifier) AS object_name,
           CAST('COLLATION' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST('NO' AS yes_or_no) AS is_grantable

    FROM pg_authid u,
         pg_namespace n,
         pg_collation c

    WHERE u.oid = c.collowner
          AND c.collnamespace = n.oid
          AND collencoding IN (-1, (SELECT encoding FROM pg_database WHERE datname = current_database()))

    UNION ALL

    /* domains */
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(t.typname AS sql_identifier) AS object_name,
           CAST('DOMAIN' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, t.typowner, 'USAGE')
                  OR t.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT oid, typname, typnamespace, typtype, typowner, (aclexplode(coalesce(typacl, acldefault('T', typowner)))).* FROM pg_type
         ) AS t (oid, typname, typnamespace, typtype, typowner, grantor, grantee, prtype, grantable),
         pg_namespace n,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE t.typnamespace = n.oid
          AND t.typtype = 'd'
          AND t.grantee = grantee.oid
          AND t.grantor = u_grantor.oid
          AND t.prtype IN ('USAGE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC')

    UNION ALL

    /* foreign-data wrappers */
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST('' AS sql_identifier) AS object_schema,
           CAST(fdw.fdwname AS sql_identifier) AS object_name,
           CAST('FOREIGN DATA WRAPPER' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, fdw.fdwowner, 'USAGE')
                  OR fdw.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT fdwname, fdwowner, (aclexplode(coalesce(fdwacl, acldefault('F', fdwowner)))).* FROM pg_foreign_data_wrapper
         ) AS fdw (fdwname, fdwowner, grantor, grantee, prtype, grantable),
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE u_grantor.oid = fdw.grantor
          AND grantee.oid = fdw.grantee
          AND fdw.prtype IN ('USAGE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC')

    UNION ALL

    /* foreign servers */
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST('' AS sql_identifier) AS object_schema,
           CAST(srv.srvname AS sql_identifier) AS object_name,
           CAST('FOREIGN SERVER' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, srv.srvowner, 'USAGE')
                  OR srv.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT srvname, srvowner, (aclexplode(coalesce(srvacl, acldefault('S', srvowner)))).* FROM pg_foreign_server
         ) AS srv (srvname, srvowner, grantor, grantee, prtype, grantable),
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE u_grantor.oid = srv.grantor
          AND grantee.oid = srv.grantee
          AND srv.prtype IN ('USAGE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC')

    UNION ALL

    /* sequences */
    SELECT CAST(u_grantor.rolname AS sql_identifier) AS grantor,
           CAST(grantee.rolname AS sql_identifier) AS grantee,
           CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(c.relname AS sql_identifier) AS object_name,
           CAST('SEQUENCE' AS character_data) AS object_type,
           CAST('USAGE' AS character_data) AS privilege_type,
           CAST(
             CASE WHEN
                  -- object owner always has grant options
                  pg_has_role(grantee.oid, c.relowner, 'USAGE')
                  OR c.grantable
                  THEN 'YES' ELSE 'NO' END AS yes_or_no) AS is_grantable

    FROM (
            SELECT oid, relname, relnamespace, relkind, relowner, (aclexplode(coalesce(relacl, acldefault('r', relowner)))).* FROM pg_class
         ) AS c (oid, relname, relnamespace, relkind, relowner, grantor, grantee, prtype, grantable),
         pg_namespace n,
         pg_authid u_grantor,
         (
           SELECT oid, rolname FROM pg_authid
           UNION ALL
           SELECT 0::oid, 'PUBLIC'
         ) AS grantee (oid, rolname)

    WHERE c.relnamespace = n.oid
          AND c.relkind = 'S'
          AND c.grantee = grantee.oid
          AND c.grantor = u_grantor.oid
          AND c.prtype IN ('USAGE')
          AND (pg_has_role(u_grantor.oid, 'USAGE')
               OR pg_has_role(grantee.oid, 'USAGE')
               OR grantee.rolname = 'PUBLIC');

GRANT SELECT ON usage_privileges TO PUBLIC;


/*
 * 5.45
 * ROLE_USAGE_GRANTS view
 */

CREATE VIEW role_usage_grants AS
    SELECT grantor,
           grantee,
           object_catalog,
           object_schema,
           object_name,
           object_type,
           privilege_type,
           is_grantable
    FROM usage_privileges
    WHERE grantor IN (SELECT role_name FROM enabled_roles)
          OR grantee IN (SELECT role_name FROM enabled_roles);

GRANT SELECT ON role_usage_grants TO PUBLIC;


/*
 * 5.76
 * USER_DEFINED_TYPES view
 */

CREATE VIEW user_defined_types AS
    SELECT CAST(current_database() AS sql_identifier) AS user_defined_type_catalog,
           CAST(n.nspname AS sql_identifier) AS user_defined_type_schema,
           CAST(c.relname AS sql_identifier) AS user_defined_type_name,
           CAST('STRUCTURED' AS character_data) AS user_defined_type_category,
           CAST('YES' AS yes_or_no) AS is_instantiable,
           CAST(null AS yes_or_no) AS is_final,
           CAST(null AS character_data) AS ordering_form,
           CAST(null AS character_data) AS ordering_category,
           CAST(null AS sql_identifier) AS ordering_routine_catalog,
           CAST(null AS sql_identifier) AS ordering_routine_schema,
           CAST(null AS sql_identifier) AS ordering_routine_name,
           CAST(null AS character_data) AS reference_type,
           CAST(null AS character_data) AS data_type,
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
           CAST(null AS cardinal_number) AS interval_precision,
           CAST(null AS sql_identifier) AS source_dtd_identifier,
           CAST(null AS sql_identifier) AS ref_dtd_identifier

    FROM pg_namespace n, pg_class c, pg_type t

    WHERE n.oid = c.relnamespace
          AND t.typrelid = c.oid
          AND c.relkind = 'c'
          AND (pg_has_role(t.typowner, 'USAGE')
               OR has_type_privilege(t.oid, 'USAGE'));

GRANT SELECT ON user_defined_types TO PUBLIC;


/*
 * 5.77
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

    FROM pg_namespace nv, pg_class v, pg_depend dv,
         pg_depend dt, pg_class t, pg_namespace nt,
         pg_attribute a

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
          AND t.relkind IN ('r', 'v', 'f', 'p')
          AND t.oid = a.attrelid
          AND dt.refobjsubid = a.attnum
          AND pg_has_role(t.relowner, 'USAGE');

GRANT SELECT ON view_column_usage TO PUBLIC;


/*
 * 5.78
 * VIEW_PERIOD_USAGE
 */

-- feature not supported


/*
 * 5.79
 * VIEW_ROUTINE_USAGE
 */

CREATE VIEW view_routine_usage AS
    SELECT DISTINCT
           CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nv.nspname AS sql_identifier) AS table_schema,
           CAST(v.relname AS sql_identifier) AS table_name,
           CAST(current_database() AS sql_identifier) AS specific_catalog,
           CAST(np.nspname AS sql_identifier) AS specific_schema,
           CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier) AS specific_name

    FROM pg_namespace nv, pg_class v, pg_depend dv,
         pg_depend dp, pg_proc p, pg_namespace np

    WHERE nv.oid = v.relnamespace
          AND v.relkind = 'v'
          AND v.oid = dv.refobjid
          AND dv.refclassid = 'pg_catalog.pg_class'::regclass
          AND dv.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dv.deptype = 'i'
          AND dv.objid = dp.objid
          AND dp.classid = 'pg_catalog.pg_rewrite'::regclass
          AND dp.refclassid = 'pg_catalog.pg_proc'::regclass
          AND dp.refobjid = p.oid
          AND p.pronamespace = np.oid
          AND pg_has_role(p.proowner, 'USAGE');

GRANT SELECT ON view_routine_usage TO PUBLIC;


/*
 * 5.80
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

    FROM pg_namespace nv, pg_class v, pg_depend dv,
         pg_depend dt, pg_class t, pg_namespace nt

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
          AND t.relkind IN ('r', 'v', 'f', 'p')
          AND pg_has_role(t.relowner, 'USAGE');

GRANT SELECT ON view_table_usage TO PUBLIC;


/*
 * 5.81
 * VIEWS view
 */

CREATE VIEW views AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(nc.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,

           CAST(
             CASE WHEN pg_has_role(c.relowner, 'USAGE')
                  THEN pg_get_viewdef(c.oid)
                  ELSE null END
             AS character_data) AS view_definition,

           CAST(
             CASE WHEN 'check_option=cascaded' = ANY (c.reloptions)
                  THEN 'CASCADED'
                  WHEN 'check_option=local' = ANY (c.reloptions)
                  THEN 'LOCAL'
                  ELSE 'NONE' END
             AS character_data) AS check_option,

           CAST(
             -- (1 << CMD_UPDATE) + (1 << CMD_DELETE)
             CASE WHEN pg_relation_is_updatable(c.oid, false) & 20 = 20
                  THEN 'YES' ELSE 'NO' END
             AS yes_or_no) AS is_updatable,

           CAST(
             -- 1 << CMD_INSERT
             CASE WHEN pg_relation_is_updatable(c.oid, false) & 8 = 8
                  THEN 'YES' ELSE 'NO' END
             AS yes_or_no) AS is_insertable_into,

           CAST(
             -- TRIGGER_TYPE_ROW + TRIGGER_TYPE_INSTEAD + TRIGGER_TYPE_UPDATE
             CASE WHEN EXISTS (SELECT 1 FROM pg_trigger WHERE tgrelid = c.oid AND tgtype & 81 = 81)
                  THEN 'YES' ELSE 'NO' END
           AS yes_or_no) AS is_trigger_updatable,

           CAST(
             -- TRIGGER_TYPE_ROW + TRIGGER_TYPE_INSTEAD + TRIGGER_TYPE_DELETE
             CASE WHEN EXISTS (SELECT 1 FROM pg_trigger WHERE tgrelid = c.oid AND tgtype & 73 = 73)
                  THEN 'YES' ELSE 'NO' END
           AS yes_or_no) AS is_trigger_deletable,

           CAST(
             -- TRIGGER_TYPE_ROW + TRIGGER_TYPE_INSTEAD + TRIGGER_TYPE_INSERT
             CASE WHEN EXISTS (SELECT 1 FROM pg_trigger WHERE tgrelid = c.oid AND tgtype & 69 = 69)
                  THEN 'YES' ELSE 'NO' END
           AS yes_or_no) AS is_trigger_insertable_into

    FROM pg_namespace nc, pg_class c

    WHERE c.relnamespace = nc.oid
          AND c.relkind = 'v'
          AND (NOT pg_is_other_temp_schema(nc.oid))
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(c.oid, 'SELECT, INSERT, UPDATE, REFERENCES') );

GRANT SELECT ON views TO PUBLIC;


-- The following views have dependencies that force them to appear out of order.

/*
 * 5.25
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
        SELECT udt_schema, udt_name, 'USER-DEFINED TYPE'::text, dtd_identifier FROM attributes
        UNION ALL
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
 * 5.30
 * ELEMENT_TYPES view
 */

CREATE VIEW element_types AS
    SELECT CAST(current_database() AS sql_identifier) AS object_catalog,
           CAST(n.nspname AS sql_identifier) AS object_schema,
           CAST(x.objname AS sql_identifier) AS object_name,
           CAST(x.objtype AS character_data) AS object_type,
           CAST(x.objdtdid AS sql_identifier) AS collection_type_identifier,
           CAST(
             CASE WHEN nbt.nspname = 'pg_catalog' THEN format_type(bt.oid, null)
                  ELSE 'USER-DEFINED' END AS character_data) AS data_type,

           CAST(null AS cardinal_number) AS character_maximum_length,
           CAST(null AS cardinal_number) AS character_octet_length,
           CAST(null AS sql_identifier) AS character_set_catalog,
           CAST(null AS sql_identifier) AS character_set_schema,
           CAST(null AS sql_identifier) AS character_set_name,
           CAST(CASE WHEN nco.nspname IS NOT NULL THEN current_database() END AS sql_identifier) AS collation_catalog,
           CAST(nco.nspname AS sql_identifier) AS collation_schema,
           CAST(co.collname AS sql_identifier) AS collation_name,
           CAST(null AS cardinal_number) AS numeric_precision,
           CAST(null AS cardinal_number) AS numeric_precision_radix,
           CAST(null AS cardinal_number) AS numeric_scale,
           CAST(null AS cardinal_number) AS datetime_precision,
           CAST(null AS character_data) AS interval_type,
           CAST(null AS cardinal_number) AS interval_precision,

           CAST(null AS character_data) AS domain_default, -- XXX maybe a bug in the standard

           CAST(current_database() AS sql_identifier) AS udt_catalog,
           CAST(nbt.nspname AS sql_identifier) AS udt_schema,
           CAST(bt.typname AS sql_identifier) AS udt_name,

           CAST(null AS sql_identifier) AS scope_catalog,
           CAST(null AS sql_identifier) AS scope_schema,
           CAST(null AS sql_identifier) AS scope_name,

           CAST(null AS cardinal_number) AS maximum_cardinality,
           CAST('a' || CAST(x.objdtdid AS text) AS sql_identifier) AS dtd_identifier

    FROM pg_namespace n, pg_type at, pg_namespace nbt, pg_type bt,
         (
           /* columns, attributes */
           SELECT c.relnamespace, CAST(c.relname AS sql_identifier),
                  CASE WHEN c.relkind = 'c' THEN 'USER-DEFINED TYPE'::text ELSE 'TABLE'::text END,
                  a.attnum, a.atttypid, a.attcollation
           FROM pg_class c, pg_attribute a
           WHERE c.oid = a.attrelid
                 AND c.relkind IN ('r', 'v', 'f', 'c', 'p')
                 AND attnum > 0 AND NOT attisdropped

           UNION ALL

           /* domains */
           SELECT t.typnamespace, CAST(t.typname AS sql_identifier),
                  'DOMAIN'::text, 1, t.typbasetype, t.typcollation
           FROM pg_type t
           WHERE t.typtype = 'd'

           UNION ALL

           /* parameters */
           SELECT pronamespace,
                  CAST(nameconcatoid(proname, oid) AS sql_identifier),
                  'ROUTINE'::text, (ss.x).n, (ss.x).x, 0
           FROM (SELECT p.pronamespace, p.proname, p.oid,
                        _pg_expandarray(coalesce(p.proallargtypes, p.proargtypes::oid[])) AS x
                 FROM pg_proc p) AS ss

           UNION ALL

           /* result types */
           SELECT p.pronamespace,
                  CAST(nameconcatoid(p.proname, p.oid) AS sql_identifier),
                  'ROUTINE'::text, 0, p.prorettype, 0
           FROM pg_proc p

         ) AS x (objschema, objname, objtype, objdtdid, objtypeid, objcollation)
         LEFT JOIN (pg_collation co JOIN pg_namespace nco ON (co.collnamespace = nco.oid))
           ON x.objcollation = co.oid AND (nco.nspname, co.collname) <> ('pg_catalog', 'default')

    WHERE n.oid = x.objschema
          AND at.oid = x.objtypeid
          AND (at.typelem <> 0 AND at.typlen = -1)
          AND at.typelem = bt.oid
          AND nbt.oid = bt.typnamespace

          AND (n.nspname, x.objname, x.objtype, CAST(x.objdtdid AS sql_identifier)) IN
              ( SELECT object_schema, object_name, object_type, dtd_identifier
                    FROM data_type_privileges );

GRANT SELECT ON element_types TO PUBLIC;


-- SQL/MED views; these use section numbers from part 9 of the standard.

/* Base view for foreign table columns */
CREATE VIEW _pg_foreign_table_columns AS
    SELECT n.nspname,
           c.relname,
           a.attname,
           a.attfdwoptions
    FROM pg_foreign_table t, pg_authid u, pg_namespace n, pg_class c,
         pg_attribute a
    WHERE u.oid = c.relowner
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_column_privilege(c.oid, a.attnum, 'SELECT, INSERT, UPDATE, REFERENCES'))
          AND n.oid = c.relnamespace
          AND c.oid = t.ftrelid
          AND c.relkind = 'f'
          AND a.attrelid = c.oid
          AND a.attnum > 0;

/*
 * 24.2
 * COLUMN_OPTIONS view
 */
CREATE VIEW column_options AS
    SELECT CAST(current_database() AS sql_identifier) AS table_catalog,
           CAST(c.nspname AS sql_identifier) AS table_schema,
           CAST(c.relname AS sql_identifier) AS table_name,
           CAST(c.attname AS sql_identifier) AS column_name,
           CAST((pg_options_to_table(c.attfdwoptions)).option_name AS sql_identifier) AS option_name,
           CAST((pg_options_to_table(c.attfdwoptions)).option_value AS character_data) AS option_value
    FROM _pg_foreign_table_columns c;

GRANT SELECT ON column_options TO PUBLIC;


/* Base view for foreign-data wrappers */
CREATE VIEW _pg_foreign_data_wrappers AS
    SELECT w.oid,
           w.fdwowner,
           w.fdwoptions,
           CAST(current_database() AS sql_identifier) AS foreign_data_wrapper_catalog,
           CAST(fdwname AS sql_identifier) AS foreign_data_wrapper_name,
           CAST(u.rolname AS sql_identifier) AS authorization_identifier,
           CAST('c' AS character_data) AS foreign_data_wrapper_language
    FROM pg_foreign_data_wrapper w, pg_authid u
    WHERE u.oid = w.fdwowner
          AND (pg_has_role(fdwowner, 'USAGE')
               OR has_foreign_data_wrapper_privilege(w.oid, 'USAGE'));


/*
 * 24.4
 * FOREIGN_DATA_WRAPPER_OPTIONS view
 */
CREATE VIEW foreign_data_wrapper_options AS
    SELECT foreign_data_wrapper_catalog,
           foreign_data_wrapper_name,
           CAST((pg_options_to_table(w.fdwoptions)).option_name AS sql_identifier) AS option_name,
           CAST((pg_options_to_table(w.fdwoptions)).option_value AS character_data) AS option_value
    FROM _pg_foreign_data_wrappers w;

GRANT SELECT ON foreign_data_wrapper_options TO PUBLIC;


/*
 * 24.5
 * FOREIGN_DATA_WRAPPERS view
 */
CREATE VIEW foreign_data_wrappers AS
    SELECT foreign_data_wrapper_catalog,
           foreign_data_wrapper_name,
           authorization_identifier,
           CAST(NULL AS character_data) AS library_name,
           foreign_data_wrapper_language
    FROM _pg_foreign_data_wrappers w;

GRANT SELECT ON foreign_data_wrappers TO PUBLIC;


/* Base view for foreign servers */
CREATE VIEW _pg_foreign_servers AS
    SELECT s.oid,
           s.srvoptions,
           CAST(current_database() AS sql_identifier) AS foreign_server_catalog,
           CAST(srvname AS sql_identifier) AS foreign_server_name,
           CAST(current_database() AS sql_identifier) AS foreign_data_wrapper_catalog,
           CAST(w.fdwname AS sql_identifier) AS foreign_data_wrapper_name,
           CAST(srvtype AS character_data) AS foreign_server_type,
           CAST(srvversion AS character_data) AS foreign_server_version,
           CAST(u.rolname AS sql_identifier) AS authorization_identifier
    FROM pg_foreign_server s, pg_foreign_data_wrapper w, pg_authid u
    WHERE w.oid = s.srvfdw
          AND u.oid = s.srvowner
          AND (pg_has_role(s.srvowner, 'USAGE')
               OR has_server_privilege(s.oid, 'USAGE'));


/*
 * 24.6
 * FOREIGN_SERVER_OPTIONS view
 */
CREATE VIEW foreign_server_options AS
    SELECT foreign_server_catalog,
           foreign_server_name,
           CAST((pg_options_to_table(s.srvoptions)).option_name AS sql_identifier) AS option_name,
           CAST((pg_options_to_table(s.srvoptions)).option_value AS character_data) AS option_value
    FROM _pg_foreign_servers s;

GRANT SELECT ON TABLE foreign_server_options TO PUBLIC;


/*
 * 24.7
 * FOREIGN_SERVERS view
 */
CREATE VIEW foreign_servers AS
    SELECT foreign_server_catalog,
           foreign_server_name,
           foreign_data_wrapper_catalog,
           foreign_data_wrapper_name,
           foreign_server_type,
           foreign_server_version,
           authorization_identifier
    FROM _pg_foreign_servers;

GRANT SELECT ON foreign_servers TO PUBLIC;


/* Base view for foreign tables */
CREATE VIEW _pg_foreign_tables AS
    SELECT
           CAST(current_database() AS sql_identifier) AS foreign_table_catalog,
           CAST(n.nspname AS sql_identifier) AS foreign_table_schema,
           CAST(c.relname AS sql_identifier) AS foreign_table_name,
           t.ftoptions AS ftoptions,
           CAST(current_database() AS sql_identifier) AS foreign_server_catalog,
           CAST(srvname AS sql_identifier) AS foreign_server_name,
           CAST(u.rolname AS sql_identifier) AS authorization_identifier
    FROM pg_foreign_table t, pg_foreign_server s, pg_foreign_data_wrapper w,
         pg_authid u, pg_namespace n, pg_class c
    WHERE w.oid = s.srvfdw
          AND u.oid = c.relowner
          AND (pg_has_role(c.relowner, 'USAGE')
               OR has_table_privilege(c.oid, 'SELECT, INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER')
               OR has_any_column_privilege(c.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
          AND n.oid = c.relnamespace
          AND c.oid = t.ftrelid
          AND c.relkind = 'f'
          AND s.oid = t.ftserver;


/*
 * 24.8
 * FOREIGN_TABLE_OPTIONS view
 */
CREATE VIEW foreign_table_options AS
    SELECT foreign_table_catalog,
           foreign_table_schema,
           foreign_table_name,
           CAST((pg_options_to_table(t.ftoptions)).option_name AS sql_identifier) AS option_name,
           CAST((pg_options_to_table(t.ftoptions)).option_value AS character_data) AS option_value
    FROM _pg_foreign_tables t;

GRANT SELECT ON TABLE foreign_table_options TO PUBLIC;


/*
 * 24.9
 * FOREIGN_TABLES view
 */
CREATE VIEW foreign_tables AS
    SELECT foreign_table_catalog,
           foreign_table_schema,
           foreign_table_name,
           foreign_server_catalog,
           foreign_server_name
    FROM _pg_foreign_tables;

GRANT SELECT ON foreign_tables TO PUBLIC;



/* Base view for user mappings */
CREATE VIEW _pg_user_mappings AS
    SELECT um.oid,
           um.umoptions,
           um.umuser,
           CAST(COALESCE(u.rolname,'PUBLIC') AS sql_identifier ) AS authorization_identifier,
           s.foreign_server_catalog,
           s.foreign_server_name,
           s.authorization_identifier AS srvowner
    FROM pg_user_mapping um LEFT JOIN pg_authid u ON (u.oid = um.umuser),
         _pg_foreign_servers s
    WHERE s.oid = um.umserver;


/*
 * 24.12
 * USER_MAPPING_OPTIONS view
 */
CREATE VIEW user_mapping_options AS
    SELECT authorization_identifier,
           foreign_server_catalog,
           foreign_server_name,
           CAST(opts.option_name AS sql_identifier) AS option_name,
           CAST(CASE WHEN (umuser <> 0 AND authorization_identifier = current_user)
                       OR (umuser = 0 AND pg_has_role(srvowner, 'USAGE'))
                       OR (SELECT rolsuper FROM pg_authid WHERE rolname = current_user)
                     THEN opts.option_value
                     ELSE NULL END AS character_data) AS option_value
    FROM _pg_user_mappings um,
         pg_options_to_table(um.umoptions) opts;

GRANT SELECT ON user_mapping_options TO PUBLIC;


/*
 * 24.13
 * USER_MAPPINGS view
 */
CREATE VIEW user_mappings AS
    SELECT authorization_identifier,
           foreign_server_catalog,
           foreign_server_name
    FROM _pg_user_mappings;

GRANT SELECT ON user_mappings TO PUBLIC;
