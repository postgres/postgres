/*
 * PostgreSQL System Views
 *
 * Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_views.sql
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
 */

CREATE VIEW pg_roles AS
    SELECT
        rolname,
        rolsuper,
        rolinherit,
        rolcreaterole,
        rolcreatedb,
        rolcanlogin,
        rolreplication,
        rolconnlimit,
        '********'::text as rolpassword,
        rolvaliduntil,
        rolbypassrls,
        setconfig as rolconfig,
        pg_authid.oid
    FROM pg_authid LEFT JOIN pg_db_role_setting s
    ON (pg_authid.oid = setrole AND setdatabase = 0);

CREATE VIEW pg_shadow AS
    SELECT
        rolname AS usename,
        pg_authid.oid AS usesysid,
        rolcreatedb AS usecreatedb,
        rolsuper AS usesuper,
        rolreplication AS userepl,
        rolbypassrls AS usebypassrls,
        rolpassword AS passwd,
        rolvaliduntil AS valuntil,
        setconfig AS useconfig
    FROM pg_authid LEFT JOIN pg_db_role_setting s
    ON (pg_authid.oid = setrole AND setdatabase = 0)
    WHERE rolcanlogin;

REVOKE ALL ON pg_shadow FROM public;

CREATE VIEW pg_group AS
    SELECT
        rolname AS groname,
        oid AS grosysid,
        ARRAY(SELECT member FROM pg_auth_members WHERE roleid = pg_authid.oid) AS grolist
    FROM pg_authid
    WHERE NOT rolcanlogin;

CREATE VIEW pg_user AS
    SELECT
        usename,
        usesysid,
        usecreatedb,
        usesuper,
        userepl,
        usebypassrls,
        '********'::text as passwd,
        valuntil,
        useconfig
    FROM pg_shadow;

CREATE VIEW pg_policies AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        pol.polname AS policyname,
        CASE
            WHEN pol.polpermissive THEN
                'PERMISSIVE'
            ELSE
                'RESTRICTIVE'
        END AS permissive,
        CASE
            WHEN pol.polroles = '{0}' THEN
                string_to_array('public', '')
            ELSE
                ARRAY
                (
                    SELECT rolname
                    FROM pg_catalog.pg_authid
                    WHERE oid = ANY (pol.polroles) ORDER BY 1
                )
        END AS roles,
        CASE pol.polcmd
            WHEN 'r' THEN 'SELECT'
            WHEN 'a' THEN 'INSERT'
            WHEN 'w' THEN 'UPDATE'
            WHEN 'd' THEN 'DELETE'
            WHEN '*' THEN 'ALL'
        END AS cmd,
        pg_catalog.pg_get_expr(pol.polqual, pol.polrelid) AS qual,
        pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid) AS with_check
    FROM pg_catalog.pg_policy pol
    JOIN pg_catalog.pg_class C ON (C.oid = pol.polrelid)
    LEFT JOIN pg_catalog.pg_namespace N ON (N.oid = C.relnamespace);

CREATE VIEW pg_rules AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        R.rulename AS rulename,
        pg_get_ruledef(R.oid) AS definition
    FROM (pg_rewrite R JOIN pg_class C ON (C.oid = R.ev_class))
        LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE R.rulename != '_RETURN';

CREATE VIEW pg_views AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS viewname,
        pg_get_userbyid(C.relowner) AS viewowner,
        pg_get_viewdef(C.oid) AS definition
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind = 'v';

CREATE VIEW pg_tables AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        pg_get_userbyid(C.relowner) AS tableowner,
        T.spcname AS tablespace,
        C.relhasindex AS hasindexes,
        C.relhasrules AS hasrules,
        C.relhastriggers AS hastriggers,
        C.relrowsecurity AS rowsecurity
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
         LEFT JOIN pg_tablespace T ON (T.oid = C.reltablespace)
    WHERE C.relkind IN ('r', 'p');

CREATE VIEW pg_matviews AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS matviewname,
        pg_get_userbyid(C.relowner) AS matviewowner,
        T.spcname AS tablespace,
        C.relhasindex AS hasindexes,
        C.relispopulated AS ispopulated,
        pg_get_viewdef(C.oid) AS definition
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
         LEFT JOIN pg_tablespace T ON (T.oid = C.reltablespace)
    WHERE C.relkind = 'm';

CREATE VIEW pg_indexes AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS tablename,
        I.relname AS indexname,
        T.spcname AS tablespace,
        pg_get_indexdef(I.oid) AS indexdef
    FROM pg_index X JOIN pg_class C ON (C.oid = X.indrelid)
         JOIN pg_class I ON (I.oid = X.indexrelid)
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
         LEFT JOIN pg_tablespace T ON (T.oid = I.reltablespace)
    WHERE C.relkind IN ('r', 'm', 'p') AND I.relkind IN ('i', 'I');

CREATE VIEW pg_sequences AS
    SELECT
        N.nspname AS schemaname,
        C.relname AS sequencename,
        pg_get_userbyid(C.relowner) AS sequenceowner,
        S.seqtypid::regtype AS data_type,
        S.seqstart AS start_value,
        S.seqmin AS min_value,
        S.seqmax AS max_value,
        S.seqincrement AS increment_by,
        S.seqcycle AS cycle,
        S.seqcache AS cache_size,
        pg_sequence_last_value(C.oid) AS last_value
    FROM pg_sequence S JOIN pg_class C ON (C.oid = S.seqrelid)
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE NOT pg_is_other_temp_schema(N.oid)
          AND relkind = 'S';

CREATE VIEW pg_stats WITH (security_barrier) AS
    SELECT
        nspname AS schemaname,
        relname AS tablename,
        attname AS attname,
        stainherit AS inherited,
        stanullfrac AS null_frac,
        stawidth AS avg_width,
        stadistinct AS n_distinct,
        CASE
            WHEN stakind1 = 1 THEN stavalues1
            WHEN stakind2 = 1 THEN stavalues2
            WHEN stakind3 = 1 THEN stavalues3
            WHEN stakind4 = 1 THEN stavalues4
            WHEN stakind5 = 1 THEN stavalues5
        END AS most_common_vals,
        CASE
            WHEN stakind1 = 1 THEN stanumbers1
            WHEN stakind2 = 1 THEN stanumbers2
            WHEN stakind3 = 1 THEN stanumbers3
            WHEN stakind4 = 1 THEN stanumbers4
            WHEN stakind5 = 1 THEN stanumbers5
        END AS most_common_freqs,
        CASE
            WHEN stakind1 = 2 THEN stavalues1
            WHEN stakind2 = 2 THEN stavalues2
            WHEN stakind3 = 2 THEN stavalues3
            WHEN stakind4 = 2 THEN stavalues4
            WHEN stakind5 = 2 THEN stavalues5
        END AS histogram_bounds,
        CASE
            WHEN stakind1 = 3 THEN stanumbers1[1]
            WHEN stakind2 = 3 THEN stanumbers2[1]
            WHEN stakind3 = 3 THEN stanumbers3[1]
            WHEN stakind4 = 3 THEN stanumbers4[1]
            WHEN stakind5 = 3 THEN stanumbers5[1]
        END AS correlation,
        CASE
            WHEN stakind1 = 4 THEN stavalues1
            WHEN stakind2 = 4 THEN stavalues2
            WHEN stakind3 = 4 THEN stavalues3
            WHEN stakind4 = 4 THEN stavalues4
            WHEN stakind5 = 4 THEN stavalues5
        END AS most_common_elems,
        CASE
            WHEN stakind1 = 4 THEN stanumbers1
            WHEN stakind2 = 4 THEN stanumbers2
            WHEN stakind3 = 4 THEN stanumbers3
            WHEN stakind4 = 4 THEN stanumbers4
            WHEN stakind5 = 4 THEN stanumbers5
        END AS most_common_elem_freqs,
        CASE
            WHEN stakind1 = 5 THEN stanumbers1
            WHEN stakind2 = 5 THEN stanumbers2
            WHEN stakind3 = 5 THEN stanumbers3
            WHEN stakind4 = 5 THEN stanumbers4
            WHEN stakind5 = 5 THEN stanumbers5
        END AS elem_count_histogram,
        CASE
            WHEN stakind1 = 6 THEN stavalues1
            WHEN stakind2 = 6 THEN stavalues2
            WHEN stakind3 = 6 THEN stavalues3
            WHEN stakind4 = 6 THEN stavalues4
            WHEN stakind5 = 6 THEN stavalues5
        END AS range_length_histogram,
        CASE
            WHEN stakind1 = 6 THEN stanumbers1[1]
            WHEN stakind2 = 6 THEN stanumbers2[1]
            WHEN stakind3 = 6 THEN stanumbers3[1]
            WHEN stakind4 = 6 THEN stanumbers4[1]
            WHEN stakind5 = 6 THEN stanumbers5[1]
        END AS range_empty_frac,
        CASE
            WHEN stakind1 = 7 THEN stavalues1
            WHEN stakind2 = 7 THEN stavalues2
            WHEN stakind3 = 7 THEN stavalues3
            WHEN stakind4 = 7 THEN stavalues4
            WHEN stakind5 = 7 THEN stavalues5
            END AS range_bounds_histogram
    FROM pg_statistic s JOIN pg_class c ON (c.oid = s.starelid)
         JOIN pg_attribute a ON (c.oid = attrelid AND attnum = s.staattnum)
         LEFT JOIN pg_namespace n ON (n.oid = c.relnamespace)
    WHERE NOT attisdropped
    AND has_column_privilege(c.oid, a.attnum, 'select')
    AND (c.relrowsecurity = false OR NOT row_security_active(c.oid));

REVOKE ALL ON pg_statistic FROM public;

CREATE VIEW pg_stats_ext WITH (security_barrier) AS
    SELECT cn.nspname AS schemaname,
           c.relname AS tablename,
           sn.nspname AS statistics_schemaname,
           s.stxname AS statistics_name,
           pg_get_userbyid(s.stxowner) AS statistics_owner,
           ( SELECT array_agg(a.attname ORDER BY a.attnum)
             FROM unnest(s.stxkeys) k
                  JOIN pg_attribute a
                       ON (a.attrelid = s.stxrelid AND a.attnum = k)
           ) AS attnames,
           pg_get_statisticsobjdef_expressions(s.oid) as exprs,
           s.stxkind AS kinds,
           sd.stxdinherit AS inherited,
           sd.stxdndistinct AS n_distinct,
           sd.stxddependencies AS dependencies,
           m.most_common_vals,
           m.most_common_val_nulls,
           m.most_common_freqs,
           m.most_common_base_freqs
    FROM pg_statistic_ext s JOIN pg_class c ON (c.oid = s.stxrelid)
         JOIN pg_statistic_ext_data sd ON (s.oid = sd.stxoid)
         LEFT JOIN pg_namespace cn ON (cn.oid = c.relnamespace)
         LEFT JOIN pg_namespace sn ON (sn.oid = s.stxnamespace)
         LEFT JOIN LATERAL
                   ( SELECT array_agg(values) AS most_common_vals,
                            array_agg(nulls) AS most_common_val_nulls,
                            array_agg(frequency) AS most_common_freqs,
                            array_agg(base_frequency) AS most_common_base_freqs
                     FROM pg_mcv_list_items(sd.stxdmcv)
                   ) m ON sd.stxdmcv IS NOT NULL
    WHERE pg_has_role(c.relowner, 'USAGE')
    AND (c.relrowsecurity = false OR NOT row_security_active(c.oid));

CREATE VIEW pg_stats_ext_exprs WITH (security_barrier) AS
    SELECT cn.nspname AS schemaname,
           c.relname AS tablename,
           sn.nspname AS statistics_schemaname,
           s.stxname AS statistics_name,
           pg_get_userbyid(s.stxowner) AS statistics_owner,
           stat.expr,
           sd.stxdinherit AS inherited,
           (stat.a).stanullfrac AS null_frac,
           (stat.a).stawidth AS avg_width,
           (stat.a).stadistinct AS n_distinct,
           (CASE
               WHEN (stat.a).stakind1 = 1 THEN (stat.a).stavalues1
               WHEN (stat.a).stakind2 = 1 THEN (stat.a).stavalues2
               WHEN (stat.a).stakind3 = 1 THEN (stat.a).stavalues3
               WHEN (stat.a).stakind4 = 1 THEN (stat.a).stavalues4
               WHEN (stat.a).stakind5 = 1 THEN (stat.a).stavalues5
           END) AS most_common_vals,
           (CASE
               WHEN (stat.a).stakind1 = 1 THEN (stat.a).stanumbers1
               WHEN (stat.a).stakind2 = 1 THEN (stat.a).stanumbers2
               WHEN (stat.a).stakind3 = 1 THEN (stat.a).stanumbers3
               WHEN (stat.a).stakind4 = 1 THEN (stat.a).stanumbers4
               WHEN (stat.a).stakind5 = 1 THEN (stat.a).stanumbers5
           END) AS most_common_freqs,
           (CASE
               WHEN (stat.a).stakind1 = 2 THEN (stat.a).stavalues1
               WHEN (stat.a).stakind2 = 2 THEN (stat.a).stavalues2
               WHEN (stat.a).stakind3 = 2 THEN (stat.a).stavalues3
               WHEN (stat.a).stakind4 = 2 THEN (stat.a).stavalues4
               WHEN (stat.a).stakind5 = 2 THEN (stat.a).stavalues5
           END) AS histogram_bounds,
           (CASE
               WHEN (stat.a).stakind1 = 3 THEN (stat.a).stanumbers1[1]
               WHEN (stat.a).stakind2 = 3 THEN (stat.a).stanumbers2[1]
               WHEN (stat.a).stakind3 = 3 THEN (stat.a).stanumbers3[1]
               WHEN (stat.a).stakind4 = 3 THEN (stat.a).stanumbers4[1]
               WHEN (stat.a).stakind5 = 3 THEN (stat.a).stanumbers5[1]
           END) correlation,
           (CASE
               WHEN (stat.a).stakind1 = 4 THEN (stat.a).stavalues1
               WHEN (stat.a).stakind2 = 4 THEN (stat.a).stavalues2
               WHEN (stat.a).stakind3 = 4 THEN (stat.a).stavalues3
               WHEN (stat.a).stakind4 = 4 THEN (stat.a).stavalues4
               WHEN (stat.a).stakind5 = 4 THEN (stat.a).stavalues5
           END) AS most_common_elems,
           (CASE
               WHEN (stat.a).stakind1 = 4 THEN (stat.a).stanumbers1
               WHEN (stat.a).stakind2 = 4 THEN (stat.a).stanumbers2
               WHEN (stat.a).stakind3 = 4 THEN (stat.a).stanumbers3
               WHEN (stat.a).stakind4 = 4 THEN (stat.a).stanumbers4
               WHEN (stat.a).stakind5 = 4 THEN (stat.a).stanumbers5
           END) AS most_common_elem_freqs,
           (CASE
               WHEN (stat.a).stakind1 = 5 THEN (stat.a).stanumbers1
               WHEN (stat.a).stakind2 = 5 THEN (stat.a).stanumbers2
               WHEN (stat.a).stakind3 = 5 THEN (stat.a).stanumbers3
               WHEN (stat.a).stakind4 = 5 THEN (stat.a).stanumbers4
               WHEN (stat.a).stakind5 = 5 THEN (stat.a).stanumbers5
           END) AS elem_count_histogram
    FROM pg_statistic_ext s JOIN pg_class c ON (c.oid = s.stxrelid)
         LEFT JOIN pg_statistic_ext_data sd ON (s.oid = sd.stxoid)
         LEFT JOIN pg_namespace cn ON (cn.oid = c.relnamespace)
         LEFT JOIN pg_namespace sn ON (sn.oid = s.stxnamespace)
         JOIN LATERAL (
             SELECT unnest(pg_get_statisticsobjdef_expressions(s.oid)) AS expr,
                    unnest(sd.stxdexpr)::pg_statistic AS a
         ) stat ON (stat.expr IS NOT NULL)
    WHERE pg_has_role(c.relowner, 'USAGE')
    AND (c.relrowsecurity = false OR NOT row_security_active(c.oid));

-- unprivileged users may read pg_statistic_ext but not pg_statistic_ext_data
REVOKE ALL ON pg_statistic_ext_data FROM public;

CREATE VIEW pg_publication_tables AS
    SELECT
        P.pubname AS pubname,
        N.nspname AS schemaname,
        C.relname AS tablename,
        ( SELECT array_agg(a.attname ORDER BY a.attnum)
          FROM pg_attribute a
          WHERE a.attrelid = GPT.relid AND
                a.attnum = ANY(GPT.attrs)
        ) AS attnames,
        pg_get_expr(GPT.qual, GPT.relid) AS rowfilter
    FROM pg_publication P,
         LATERAL pg_get_publication_tables(P.pubname) GPT,
         pg_class C JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.oid = GPT.relid;

CREATE VIEW pg_locks AS
    SELECT * FROM pg_lock_status() AS L;

CREATE VIEW pg_cursors AS
    SELECT * FROM pg_cursor() AS C;

CREATE VIEW pg_available_extensions AS
    SELECT E.name, E.default_version, X.extversion AS installed_version,
           E.comment
      FROM pg_available_extensions() AS E
           LEFT JOIN pg_extension AS X ON E.name = X.extname;

CREATE VIEW pg_available_extension_versions AS
    SELECT E.name, E.version, (X.extname IS NOT NULL) AS installed,
           E.superuser, E.trusted, E.relocatable,
           E.schema, E.requires, E.comment
      FROM pg_available_extension_versions() AS E
           LEFT JOIN pg_extension AS X
             ON E.name = X.extname AND E.version = X.extversion;

CREATE VIEW pg_prepared_xacts AS
    SELECT P.transaction, P.gid, P.prepared,
           U.rolname AS owner, D.datname AS database
    FROM pg_prepared_xact() AS P
         LEFT JOIN pg_authid U ON P.ownerid = U.oid
         LEFT JOIN pg_database D ON P.dbid = D.oid;

CREATE VIEW pg_prepared_statements AS
    SELECT * FROM pg_prepared_statement() AS P;

CREATE VIEW pg_seclabels AS
SELECT
    l.objoid, l.classoid, l.objsubid,
    CASE WHEN rel.relkind IN ('r', 'p') THEN 'table'::text
         WHEN rel.relkind = 'v' THEN 'view'::text
         WHEN rel.relkind = 'm' THEN 'materialized view'::text
         WHEN rel.relkind = 'S' THEN 'sequence'::text
         WHEN rel.relkind = 'f' THEN 'foreign table'::text END AS objtype,
    rel.relnamespace AS objnamespace,
    CASE WHEN pg_table_is_visible(rel.oid)
         THEN quote_ident(rel.relname)
         ELSE quote_ident(nsp.nspname) || '.' || quote_ident(rel.relname)
         END AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_class rel ON l.classoid = rel.tableoid AND l.objoid = rel.oid
    JOIN pg_namespace nsp ON rel.relnamespace = nsp.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'column'::text AS objtype,
    rel.relnamespace AS objnamespace,
    CASE WHEN pg_table_is_visible(rel.oid)
         THEN quote_ident(rel.relname)
         ELSE quote_ident(nsp.nspname) || '.' || quote_ident(rel.relname)
         END || '.' || att.attname AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_class rel ON l.classoid = rel.tableoid AND l.objoid = rel.oid
    JOIN pg_attribute att
         ON rel.oid = att.attrelid AND l.objsubid = att.attnum
    JOIN pg_namespace nsp ON rel.relnamespace = nsp.oid
WHERE
    l.objsubid != 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    CASE pro.prokind
            WHEN 'a' THEN 'aggregate'::text
            WHEN 'f' THEN 'function'::text
            WHEN 'p' THEN 'procedure'::text
            WHEN 'w' THEN 'window'::text END AS objtype,
    pro.pronamespace AS objnamespace,
    CASE WHEN pg_function_is_visible(pro.oid)
         THEN quote_ident(pro.proname)
         ELSE quote_ident(nsp.nspname) || '.' || quote_ident(pro.proname)
    END || '(' || pg_catalog.pg_get_function_arguments(pro.oid) || ')' AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_proc pro ON l.classoid = pro.tableoid AND l.objoid = pro.oid
    JOIN pg_namespace nsp ON pro.pronamespace = nsp.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    CASE WHEN typ.typtype = 'd' THEN 'domain'::text
    ELSE 'type'::text END AS objtype,
    typ.typnamespace AS objnamespace,
    CASE WHEN pg_type_is_visible(typ.oid)
    THEN quote_ident(typ.typname)
    ELSE quote_ident(nsp.nspname) || '.' || quote_ident(typ.typname)
    END AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_type typ ON l.classoid = typ.tableoid AND l.objoid = typ.oid
    JOIN pg_namespace nsp ON typ.typnamespace = nsp.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'large object'::text AS objtype,
    NULL::oid AS objnamespace,
    l.objoid::text AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_largeobject_metadata lom ON l.objoid = lom.oid
WHERE
    l.classoid = 'pg_catalog.pg_largeobject'::regclass AND l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'language'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(lan.lanname) AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_language lan ON l.classoid = lan.tableoid AND l.objoid = lan.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'schema'::text AS objtype,
    nsp.oid AS objnamespace,
    quote_ident(nsp.nspname) AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_namespace nsp ON l.classoid = nsp.tableoid AND l.objoid = nsp.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'event trigger'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(evt.evtname) AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_event_trigger evt ON l.classoid = evt.tableoid
        AND l.objoid = evt.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, l.objsubid,
    'publication'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(p.pubname) AS objname,
    l.provider, l.label
FROM
    pg_seclabel l
    JOIN pg_publication p ON l.classoid = p.tableoid AND l.objoid = p.oid
WHERE
    l.objsubid = 0
UNION ALL
SELECT
    l.objoid, l.classoid, 0::int4 AS objsubid,
    'subscription'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(s.subname) AS objname,
    l.provider, l.label
FROM
    pg_shseclabel l
    JOIN pg_subscription s ON l.classoid = s.tableoid AND l.objoid = s.oid
UNION ALL
SELECT
    l.objoid, l.classoid, 0::int4 AS objsubid,
    'database'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(dat.datname) AS objname,
    l.provider, l.label
FROM
    pg_shseclabel l
    JOIN pg_database dat ON l.classoid = dat.tableoid AND l.objoid = dat.oid
UNION ALL
SELECT
    l.objoid, l.classoid, 0::int4 AS objsubid,
    'tablespace'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(spc.spcname) AS objname,
    l.provider, l.label
FROM
    pg_shseclabel l
    JOIN pg_tablespace spc ON l.classoid = spc.tableoid AND l.objoid = spc.oid
UNION ALL
SELECT
    l.objoid, l.classoid, 0::int4 AS objsubid,
    'role'::text AS objtype,
    NULL::oid AS objnamespace,
    quote_ident(rol.rolname) AS objname,
    l.provider, l.label
FROM
    pg_shseclabel l
    JOIN pg_authid rol ON l.classoid = rol.tableoid AND l.objoid = rol.oid;

CREATE VIEW pg_settings AS
    SELECT * FROM pg_show_all_settings() AS A;

CREATE RULE pg_settings_u AS
    ON UPDATE TO pg_settings
    WHERE new.name = old.name DO
    SELECT set_config(old.name, new.setting, 'f');

CREATE RULE pg_settings_n AS
    ON UPDATE TO pg_settings
    DO INSTEAD NOTHING;

GRANT SELECT, UPDATE ON pg_settings TO PUBLIC;

CREATE VIEW pg_file_settings AS
   SELECT * FROM pg_show_all_file_settings() AS A;

REVOKE ALL ON pg_file_settings FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_show_all_file_settings() FROM PUBLIC;

CREATE VIEW pg_hba_file_rules AS
   SELECT * FROM pg_hba_file_rules() AS A;

REVOKE ALL ON pg_hba_file_rules FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_hba_file_rules() FROM PUBLIC;

CREATE VIEW pg_ident_file_mappings AS
   SELECT * FROM pg_ident_file_mappings() AS A;

REVOKE ALL ON pg_ident_file_mappings FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_ident_file_mappings() FROM PUBLIC;

CREATE VIEW pg_timezone_abbrevs AS
    SELECT * FROM pg_timezone_abbrevs_zone() z
    UNION ALL
    (SELECT * FROM pg_timezone_abbrevs_abbrevs() a
     WHERE NOT EXISTS (SELECT 1 FROM pg_timezone_abbrevs_zone() z2
                       WHERE z2.abbrev = a.abbrev))
    ORDER BY abbrev;

CREATE VIEW pg_timezone_names AS
    SELECT * FROM pg_timezone_names();

CREATE VIEW pg_config AS
    SELECT * FROM pg_config();

REVOKE ALL ON pg_config FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_config() FROM PUBLIC;

CREATE VIEW pg_shmem_allocations AS
    SELECT * FROM pg_get_shmem_allocations();

REVOKE ALL ON pg_shmem_allocations FROM PUBLIC;
GRANT SELECT ON pg_shmem_allocations TO pg_read_all_stats;
REVOKE EXECUTE ON FUNCTION pg_get_shmem_allocations() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_shmem_allocations() TO pg_read_all_stats;

CREATE VIEW pg_shmem_allocations_numa AS
    SELECT * FROM pg_get_shmem_allocations_numa();

REVOKE ALL ON pg_shmem_allocations_numa FROM PUBLIC;
GRANT SELECT ON pg_shmem_allocations_numa TO pg_read_all_stats;
REVOKE EXECUTE ON FUNCTION pg_get_shmem_allocations_numa() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_shmem_allocations_numa() TO pg_read_all_stats;

CREATE VIEW pg_dsm_registry_allocations AS
    SELECT * FROM pg_get_dsm_registry_allocations();

REVOKE ALL ON pg_dsm_registry_allocations FROM PUBLIC;
GRANT SELECT ON pg_dsm_registry_allocations TO pg_read_all_stats;
REVOKE EXECUTE ON FUNCTION pg_get_dsm_registry_allocations() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_dsm_registry_allocations() TO pg_read_all_stats;

CREATE VIEW pg_backend_memory_contexts AS
    SELECT * FROM pg_get_backend_memory_contexts();

REVOKE ALL ON pg_backend_memory_contexts FROM PUBLIC;
GRANT SELECT ON pg_backend_memory_contexts TO pg_read_all_stats;
REVOKE EXECUTE ON FUNCTION pg_get_backend_memory_contexts() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_backend_memory_contexts() TO pg_read_all_stats;

-- Statistics views

CREATE VIEW pg_stat_all_tables AS
    SELECT
            C.oid AS relid,
            N.nspname AS schemaname,
            C.relname AS relname,
            pg_stat_get_numscans(C.oid) AS seq_scan,
            pg_stat_get_lastscan(C.oid) AS last_seq_scan,
            pg_stat_get_tuples_returned(C.oid) AS seq_tup_read,
            sum(pg_stat_get_numscans(I.indexrelid))::bigint AS idx_scan,
            max(pg_stat_get_lastscan(I.indexrelid)) AS last_idx_scan,
            sum(pg_stat_get_tuples_fetched(I.indexrelid))::bigint +
            pg_stat_get_tuples_fetched(C.oid) AS idx_tup_fetch,
            pg_stat_get_tuples_inserted(C.oid) AS n_tup_ins,
            pg_stat_get_tuples_updated(C.oid) AS n_tup_upd,
            pg_stat_get_tuples_deleted(C.oid) AS n_tup_del,
            pg_stat_get_tuples_hot_updated(C.oid) AS n_tup_hot_upd,
            pg_stat_get_tuples_newpage_updated(C.oid) AS n_tup_newpage_upd,
            pg_stat_get_live_tuples(C.oid) AS n_live_tup,
            pg_stat_get_dead_tuples(C.oid) AS n_dead_tup,
            pg_stat_get_mod_since_analyze(C.oid) AS n_mod_since_analyze,
            pg_stat_get_ins_since_vacuum(C.oid) AS n_ins_since_vacuum,
            pg_stat_get_last_vacuum_time(C.oid) as last_vacuum,
            pg_stat_get_last_autovacuum_time(C.oid) as last_autovacuum,
            pg_stat_get_last_analyze_time(C.oid) as last_analyze,
            pg_stat_get_last_autoanalyze_time(C.oid) as last_autoanalyze,
            pg_stat_get_vacuum_count(C.oid) AS vacuum_count,
            pg_stat_get_autovacuum_count(C.oid) AS autovacuum_count,
            pg_stat_get_analyze_count(C.oid) AS analyze_count,
            pg_stat_get_autoanalyze_count(C.oid) AS autoanalyze_count,
            pg_stat_get_total_vacuum_time(C.oid) AS total_vacuum_time,
            pg_stat_get_total_autovacuum_time(C.oid) AS total_autovacuum_time,
            pg_stat_get_total_analyze_time(C.oid) AS total_analyze_time,
            pg_stat_get_total_autoanalyze_time(C.oid) AS total_autoanalyze_time
    FROM pg_class C LEFT JOIN
         pg_index I ON C.oid = I.indrelid
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm', 'p')
    GROUP BY C.oid, N.nspname, C.relname;

CREATE VIEW pg_stat_xact_all_tables AS
    SELECT
            C.oid AS relid,
            N.nspname AS schemaname,
            C.relname AS relname,
            pg_stat_get_xact_numscans(C.oid) AS seq_scan,
            pg_stat_get_xact_tuples_returned(C.oid) AS seq_tup_read,
            sum(pg_stat_get_xact_numscans(I.indexrelid))::bigint AS idx_scan,
            sum(pg_stat_get_xact_tuples_fetched(I.indexrelid))::bigint +
            pg_stat_get_xact_tuples_fetched(C.oid) AS idx_tup_fetch,
            pg_stat_get_xact_tuples_inserted(C.oid) AS n_tup_ins,
            pg_stat_get_xact_tuples_updated(C.oid) AS n_tup_upd,
            pg_stat_get_xact_tuples_deleted(C.oid) AS n_tup_del,
            pg_stat_get_xact_tuples_hot_updated(C.oid) AS n_tup_hot_upd,
            pg_stat_get_xact_tuples_newpage_updated(C.oid) AS n_tup_newpage_upd
    FROM pg_class C LEFT JOIN
         pg_index I ON C.oid = I.indrelid
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm', 'p')
    GROUP BY C.oid, N.nspname, C.relname;

CREATE VIEW pg_stat_sys_tables AS
    SELECT * FROM pg_stat_all_tables
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_xact_sys_tables AS
    SELECT * FROM pg_stat_xact_all_tables
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_user_tables AS
    SELECT * FROM pg_stat_all_tables
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_stat_xact_user_tables AS
    SELECT * FROM pg_stat_xact_all_tables
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_tables AS
    SELECT
            C.oid AS relid,
            N.nspname AS schemaname,
            C.relname AS relname,
            pg_stat_get_blocks_fetched(C.oid) -
                    pg_stat_get_blocks_hit(C.oid) AS heap_blks_read,
            pg_stat_get_blocks_hit(C.oid) AS heap_blks_hit,
            I.idx_blks_read AS idx_blks_read,
            I.idx_blks_hit AS idx_blks_hit,
            pg_stat_get_blocks_fetched(T.oid) -
                    pg_stat_get_blocks_hit(T.oid) AS toast_blks_read,
            pg_stat_get_blocks_hit(T.oid) AS toast_blks_hit,
            X.idx_blks_read AS tidx_blks_read,
            X.idx_blks_hit AS tidx_blks_hit
    FROM pg_class C LEFT JOIN
            pg_class T ON C.reltoastrelid = T.oid
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
            LEFT JOIN LATERAL (
              SELECT sum(pg_stat_get_blocks_fetched(indexrelid) -
                         pg_stat_get_blocks_hit(indexrelid))::bigint
                     AS idx_blks_read,
                     sum(pg_stat_get_blocks_hit(indexrelid))::bigint
                     AS idx_blks_hit
              FROM pg_index WHERE indrelid = C.oid ) I ON true
            LEFT JOIN LATERAL (
              SELECT sum(pg_stat_get_blocks_fetched(indexrelid) -
                         pg_stat_get_blocks_hit(indexrelid))::bigint
                     AS idx_blks_read,
                     sum(pg_stat_get_blocks_hit(indexrelid))::bigint
                     AS idx_blks_hit
              FROM pg_index WHERE indrelid = T.oid ) X ON true
    WHERE C.relkind IN ('r', 't', 'm');

CREATE VIEW pg_statio_sys_tables AS
    SELECT * FROM pg_statio_all_tables
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_tables AS
    SELECT * FROM pg_statio_all_tables
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_stat_all_indexes AS
    SELECT
            C.oid AS relid,
            I.oid AS indexrelid,
            N.nspname AS schemaname,
            C.relname AS relname,
            I.relname AS indexrelname,
            pg_stat_get_numscans(I.oid) AS idx_scan,
            pg_stat_get_lastscan(I.oid) AS last_idx_scan,
            pg_stat_get_tuples_returned(I.oid) AS idx_tup_read,
            pg_stat_get_tuples_fetched(I.oid) AS idx_tup_fetch
    FROM pg_class C JOIN
            pg_index X ON C.oid = X.indrelid JOIN
            pg_class I ON I.oid = X.indexrelid
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm');

CREATE VIEW pg_stat_sys_indexes AS
    SELECT * FROM pg_stat_all_indexes
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_user_indexes AS
    SELECT * FROM pg_stat_all_indexes
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_indexes AS
    SELECT
            C.oid AS relid,
            I.oid AS indexrelid,
            N.nspname AS schemaname,
            C.relname AS relname,
            I.relname AS indexrelname,
            pg_stat_get_blocks_fetched(I.oid) -
                    pg_stat_get_blocks_hit(I.oid) AS idx_blks_read,
            pg_stat_get_blocks_hit(I.oid) AS idx_blks_hit
    FROM pg_class C JOIN
            pg_index X ON C.oid = X.indrelid JOIN
            pg_class I ON I.oid = X.indexrelid
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm');

CREATE VIEW pg_statio_sys_indexes AS
    SELECT * FROM pg_statio_all_indexes
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_indexes AS
    SELECT * FROM pg_statio_all_indexes
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_sequences AS
    SELECT
            C.oid AS relid,
            N.nspname AS schemaname,
            C.relname AS relname,
            pg_stat_get_blocks_fetched(C.oid) -
                    pg_stat_get_blocks_hit(C.oid) AS blks_read,
            pg_stat_get_blocks_hit(C.oid) AS blks_hit
    FROM pg_class C
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind = 'S';

CREATE VIEW pg_statio_sys_sequences AS
    SELECT * FROM pg_statio_all_sequences
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_sequences AS
    SELECT * FROM pg_statio_all_sequences
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_stat_activity AS
    SELECT
            S.datid AS datid,
            D.datname AS datname,
            S.pid,
            S.leader_pid,
            S.usesysid,
            U.rolname AS usename,
            S.application_name,
            S.client_addr,
            S.client_hostname,
            S.client_port,
            S.backend_start,
            S.xact_start,
            S.query_start,
            S.state_change,
            S.wait_event_type,
            S.wait_event,
            S.state,
            S.backend_xid,
            S.backend_xmin,
            S.query_id,
            S.query,
            S.backend_type
    FROM pg_stat_get_activity(NULL) AS S
        LEFT JOIN pg_database AS D ON (S.datid = D.oid)
        LEFT JOIN pg_authid AS U ON (S.usesysid = U.oid);

CREATE VIEW pg_stat_replication AS
    SELECT
            S.pid,
            S.usesysid,
            U.rolname AS usename,
            S.application_name,
            S.client_addr,
            S.client_hostname,
            S.client_port,
            S.backend_start,
            S.backend_xmin,
            W.state,
            W.sent_lsn,
            W.write_lsn,
            W.flush_lsn,
            W.replay_lsn,
            W.write_lag,
            W.flush_lag,
            W.replay_lag,
            W.sync_priority,
            W.sync_state,
            W.reply_time
    FROM pg_stat_get_activity(NULL) AS S
        JOIN pg_stat_get_wal_senders() AS W ON (S.pid = W.pid)
        LEFT JOIN pg_authid AS U ON (S.usesysid = U.oid);

CREATE VIEW pg_stat_slru AS
    SELECT
            s.name,
            s.blks_zeroed,
            s.blks_hit,
            s.blks_read,
            s.blks_written,
            s.blks_exists,
            s.flushes,
            s.truncates,
            s.stats_reset
    FROM pg_stat_get_slru() s;

CREATE VIEW pg_stat_wal_receiver AS
    SELECT
            s.pid,
            s.status,
            s.receive_start_lsn,
            s.receive_start_tli,
            s.written_lsn,
            s.flushed_lsn,
            s.received_tli,
            s.last_msg_send_time,
            s.last_msg_receipt_time,
            s.latest_end_lsn,
            s.latest_end_time,
            s.slot_name,
            s.sender_host,
            s.sender_port,
            s.conninfo
    FROM pg_stat_get_wal_receiver() s
    WHERE s.pid IS NOT NULL;

CREATE VIEW pg_stat_recovery_prefetch AS
    SELECT
            s.stats_reset,
            s.prefetch,
            s.hit,
            s.skip_init,
            s.skip_new,
            s.skip_fpw,
            s.skip_rep,
            s.wal_distance,
            s.block_distance,
            s.io_depth
     FROM pg_stat_get_recovery_prefetch() s;

CREATE VIEW pg_stat_subscription AS
    SELECT
            su.oid AS subid,
            su.subname,
            st.worker_type,
            st.pid,
            st.leader_pid,
            st.relid,
            st.received_lsn,
            st.last_msg_send_time,
            st.last_msg_receipt_time,
            st.latest_end_lsn,
            st.latest_end_time
    FROM pg_subscription su
            LEFT JOIN pg_stat_get_subscription(NULL) st
                      ON (st.subid = su.oid);

CREATE VIEW pg_stat_ssl AS
    SELECT
            S.pid,
            S.ssl,
            S.sslversion AS version,
            S.sslcipher AS cipher,
            S.sslbits AS bits,
            S.ssl_client_dn AS client_dn,
            S.ssl_client_serial AS client_serial,
            S.ssl_issuer_dn AS issuer_dn
    FROM pg_stat_get_activity(NULL) AS S
    WHERE S.client_port IS NOT NULL;

CREATE VIEW pg_stat_gssapi AS
    SELECT
            S.pid,
            S.gss_auth AS gss_authenticated,
            S.gss_princ AS principal,
            S.gss_enc AS encrypted,
            S.gss_delegation AS credentials_delegated
    FROM pg_stat_get_activity(NULL) AS S
    WHERE S.client_port IS NOT NULL;

CREATE VIEW pg_replication_slots AS
    SELECT
            L.slot_name,
            L.plugin,
            L.slot_type,
            L.datoid,
            D.datname AS database,
            L.temporary,
            L.active,
            L.active_pid,
            L.xmin,
            L.catalog_xmin,
            L.restart_lsn,
            L.confirmed_flush_lsn,
            L.wal_status,
            L.safe_wal_size,
            L.two_phase,
            L.two_phase_at,
            L.inactive_since,
            L.conflicting,
            L.invalidation_reason,
            L.failover,
            L.synced
    FROM pg_get_replication_slots() AS L
            LEFT JOIN pg_database D ON (L.datoid = D.oid);

CREATE VIEW pg_stat_replication_slots AS
    SELECT
            s.slot_name,
            s.spill_txns,
            s.spill_count,
            s.spill_bytes,
            s.stream_txns,
            s.stream_count,
            s.stream_bytes,
            s.total_txns,
            s.total_bytes,
            s.stats_reset
    FROM pg_replication_slots as r,
        LATERAL pg_stat_get_replication_slot(slot_name) as s
    WHERE r.datoid IS NOT NULL; -- excluding physical slots

CREATE VIEW pg_stat_database AS
    SELECT
            D.oid AS datid,
            D.datname AS datname,
                CASE
                    WHEN (D.oid = (0)::oid) THEN 0
                    ELSE pg_stat_get_db_numbackends(D.oid)
                END AS numbackends,
            pg_stat_get_db_xact_commit(D.oid) AS xact_commit,
            pg_stat_get_db_xact_rollback(D.oid) AS xact_rollback,
            pg_stat_get_db_blocks_fetched(D.oid) -
                    pg_stat_get_db_blocks_hit(D.oid) AS blks_read,
            pg_stat_get_db_blocks_hit(D.oid) AS blks_hit,
            pg_stat_get_db_tuples_returned(D.oid) AS tup_returned,
            pg_stat_get_db_tuples_fetched(D.oid) AS tup_fetched,
            pg_stat_get_db_tuples_inserted(D.oid) AS tup_inserted,
            pg_stat_get_db_tuples_updated(D.oid) AS tup_updated,
            pg_stat_get_db_tuples_deleted(D.oid) AS tup_deleted,
            pg_stat_get_db_conflict_all(D.oid) AS conflicts,
            pg_stat_get_db_temp_files(D.oid) AS temp_files,
            pg_stat_get_db_temp_bytes(D.oid) AS temp_bytes,
            pg_stat_get_db_deadlocks(D.oid) AS deadlocks,
            pg_stat_get_db_checksum_failures(D.oid) AS checksum_failures,
            pg_stat_get_db_checksum_last_failure(D.oid) AS checksum_last_failure,
            pg_stat_get_db_blk_read_time(D.oid) AS blk_read_time,
            pg_stat_get_db_blk_write_time(D.oid) AS blk_write_time,
            pg_stat_get_db_session_time(D.oid) AS session_time,
            pg_stat_get_db_active_time(D.oid) AS active_time,
            pg_stat_get_db_idle_in_transaction_time(D.oid) AS idle_in_transaction_time,
            pg_stat_get_db_sessions(D.oid) AS sessions,
            pg_stat_get_db_sessions_abandoned(D.oid) AS sessions_abandoned,
            pg_stat_get_db_sessions_fatal(D.oid) AS sessions_fatal,
            pg_stat_get_db_sessions_killed(D.oid) AS sessions_killed,
            pg_stat_get_db_parallel_workers_to_launch(D.oid) as parallel_workers_to_launch,
            pg_stat_get_db_parallel_workers_launched(D.oid) as parallel_workers_launched,
            pg_stat_get_db_stat_reset_time(D.oid) AS stats_reset
    FROM (
        SELECT 0 AS oid, NULL::name AS datname
        UNION ALL
        SELECT oid, datname FROM pg_database
    ) D;

CREATE VIEW pg_stat_database_conflicts AS
    SELECT
            D.oid AS datid,
            D.datname AS datname,
            pg_stat_get_db_conflict_tablespace(D.oid) AS confl_tablespace,
            pg_stat_get_db_conflict_lock(D.oid) AS confl_lock,
            pg_stat_get_db_conflict_snapshot(D.oid) AS confl_snapshot,
            pg_stat_get_db_conflict_bufferpin(D.oid) AS confl_bufferpin,
            pg_stat_get_db_conflict_startup_deadlock(D.oid) AS confl_deadlock,
            pg_stat_get_db_conflict_logicalslot(D.oid) AS confl_active_logicalslot
    FROM pg_database D;

CREATE VIEW pg_stat_user_functions AS
    SELECT
            P.oid AS funcid,
            N.nspname AS schemaname,
            P.proname AS funcname,
            pg_stat_get_function_calls(P.oid) AS calls,
            pg_stat_get_function_total_time(P.oid) AS total_time,
            pg_stat_get_function_self_time(P.oid) AS self_time
    FROM pg_proc P LEFT JOIN pg_namespace N ON (N.oid = P.pronamespace)
    WHERE P.prolang != 12  -- fast check to eliminate built-in functions
          AND pg_stat_get_function_calls(P.oid) IS NOT NULL;

CREATE VIEW pg_stat_xact_user_functions AS
    SELECT
            P.oid AS funcid,
            N.nspname AS schemaname,
            P.proname AS funcname,
            pg_stat_get_xact_function_calls(P.oid) AS calls,
            pg_stat_get_xact_function_total_time(P.oid) AS total_time,
            pg_stat_get_xact_function_self_time(P.oid) AS self_time
    FROM pg_proc P LEFT JOIN pg_namespace N ON (N.oid = P.pronamespace)
    WHERE P.prolang != 12  -- fast check to eliminate built-in functions
          AND pg_stat_get_xact_function_calls(P.oid) IS NOT NULL;

CREATE VIEW pg_stat_archiver AS
    SELECT
        s.archived_count,
        s.last_archived_wal,
        s.last_archived_time,
        s.failed_count,
        s.last_failed_wal,
        s.last_failed_time,
        s.stats_reset
    FROM pg_stat_get_archiver() s;

CREATE VIEW pg_stat_bgwriter AS
    SELECT
        pg_stat_get_bgwriter_buf_written_clean() AS buffers_clean,
        pg_stat_get_bgwriter_maxwritten_clean() AS maxwritten_clean,
        pg_stat_get_buf_alloc() AS buffers_alloc,
        pg_stat_get_bgwriter_stat_reset_time() AS stats_reset;

CREATE VIEW pg_stat_checkpointer AS
    SELECT
        pg_stat_get_checkpointer_num_timed() AS num_timed,
        pg_stat_get_checkpointer_num_requested() AS num_requested,
        pg_stat_get_checkpointer_num_performed() AS num_done,
        pg_stat_get_checkpointer_restartpoints_timed() AS restartpoints_timed,
        pg_stat_get_checkpointer_restartpoints_requested() AS restartpoints_req,
        pg_stat_get_checkpointer_restartpoints_performed() AS restartpoints_done,
        pg_stat_get_checkpointer_write_time() AS write_time,
        pg_stat_get_checkpointer_sync_time() AS sync_time,
        pg_stat_get_checkpointer_buffers_written() AS buffers_written,
        pg_stat_get_checkpointer_slru_written() AS slru_written,
        pg_stat_get_checkpointer_stat_reset_time() AS stats_reset;

CREATE VIEW pg_stat_io AS
SELECT
       b.backend_type,
       b.object,
       b.context,
       b.reads,
       b.read_bytes,
       b.read_time,
       b.writes,
       b.write_bytes,
       b.write_time,
       b.writebacks,
       b.writeback_time,
       b.extends,
       b.extend_bytes,
       b.extend_time,
       b.hits,
       b.evictions,
       b.reuses,
       b.fsyncs,
       b.fsync_time,
       b.stats_reset
FROM pg_stat_get_io() b;

CREATE VIEW pg_stat_wal AS
    SELECT
        w.wal_records,
        w.wal_fpi,
        w.wal_bytes,
        w.wal_buffers_full,
        w.stats_reset
    FROM pg_stat_get_wal() w;

CREATE VIEW pg_stat_progress_analyze AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        CAST(S.relid AS oid) AS relid,
        CASE S.param1 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'acquiring sample rows'
                      WHEN 2 THEN 'acquiring inherited sample rows'
                      WHEN 3 THEN 'computing statistics'
                      WHEN 4 THEN 'computing extended statistics'
                      WHEN 5 THEN 'finalizing analyze'
                      END AS phase,
        S.param2 AS sample_blks_total,
        S.param3 AS sample_blks_scanned,
        S.param4 AS ext_stats_total,
        S.param5 AS ext_stats_computed,
        S.param6 AS child_tables_total,
        S.param7 AS child_tables_done,
        CAST(S.param8 AS oid) AS current_child_table_relid,
        S.param9 / 1000000::double precision AS delay_time
    FROM pg_stat_get_progress_info('ANALYZE') AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_vacuum AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        S.relid AS relid,
        CASE S.param1 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'scanning heap'
                      WHEN 2 THEN 'vacuuming indexes'
                      WHEN 3 THEN 'vacuuming heap'
                      WHEN 4 THEN 'cleaning up indexes'
                      WHEN 5 THEN 'truncating heap'
                      WHEN 6 THEN 'performing final cleanup'
                      END AS phase,
        S.param2 AS heap_blks_total, S.param3 AS heap_blks_scanned,
        S.param4 AS heap_blks_vacuumed, S.param5 AS index_vacuum_count,
        S.param6 AS max_dead_tuple_bytes, S.param7 AS dead_tuple_bytes,
        S.param8 AS num_dead_item_ids, S.param9 AS indexes_total,
        S.param10 AS indexes_processed,
        S.param11 / 1000000::double precision AS delay_time
    FROM pg_stat_get_progress_info('VACUUM') AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_cluster AS
    SELECT
        S.pid AS pid,
        S.datid AS datid,
        D.datname AS datname,
        S.relid AS relid,
        CASE S.param1 WHEN 1 THEN 'CLUSTER'
                      WHEN 2 THEN 'VACUUM FULL'
                      END AS command,
        CASE S.param2 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'seq scanning heap'
                      WHEN 2 THEN 'index scanning heap'
                      WHEN 3 THEN 'sorting tuples'
                      WHEN 4 THEN 'writing new heap'
                      WHEN 5 THEN 'swapping relation files'
                      WHEN 6 THEN 'rebuilding index'
                      WHEN 7 THEN 'performing final cleanup'
                      END AS phase,
        CAST(S.param3 AS oid) AS cluster_index_relid,
        S.param4 AS heap_tuples_scanned,
        S.param5 AS heap_tuples_written,
        S.param6 AS heap_blks_total,
        S.param7 AS heap_blks_scanned,
        S.param8 AS index_rebuild_count
    FROM pg_stat_get_progress_info('CLUSTER') AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_create_index AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        S.relid AS relid,
        CAST(S.param7 AS oid) AS index_relid,
        CASE S.param1 WHEN 1 THEN 'CREATE INDEX'
                      WHEN 2 THEN 'CREATE INDEX CONCURRENTLY'
                      WHEN 3 THEN 'REINDEX'
                      WHEN 4 THEN 'REINDEX CONCURRENTLY'
                      END AS command,
        CASE S.param10 WHEN 0 THEN 'initializing'
                       WHEN 1 THEN 'waiting for writers before build'
                       WHEN 2 THEN 'building index' ||
                           COALESCE((': ' || pg_indexam_progress_phasename(S.param9::oid, S.param11)),
                                    '')
                       WHEN 3 THEN 'waiting for writers before validation'
                       WHEN 4 THEN 'index validation: scanning index'
                       WHEN 5 THEN 'index validation: sorting tuples'
                       WHEN 6 THEN 'index validation: scanning table'
                       WHEN 7 THEN 'waiting for old snapshots'
                       WHEN 8 THEN 'waiting for readers before marking dead'
                       WHEN 9 THEN 'waiting for readers before dropping'
                       END as phase,
        S.param4 AS lockers_total,
        S.param5 AS lockers_done,
        S.param6 AS current_locker_pid,
        S.param16 AS blocks_total,
        S.param17 AS blocks_done,
        S.param12 AS tuples_total,
        S.param13 AS tuples_done,
        S.param14 AS partitions_total,
        S.param15 AS partitions_done
    FROM pg_stat_get_progress_info('CREATE INDEX') AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_stat_progress_basebackup AS
    SELECT
        S.pid AS pid,
        CASE S.param1 WHEN 0 THEN 'initializing'
                      WHEN 1 THEN 'waiting for checkpoint to finish'
                      WHEN 2 THEN 'estimating backup size'
                      WHEN 3 THEN 'streaming database files'
                      WHEN 4 THEN 'waiting for wal archiving to finish'
                      WHEN 5 THEN 'transferring wal files'
                      END AS phase,
        CASE S.param2 WHEN -1 THEN NULL ELSE S.param2 END AS backup_total,
        S.param3 AS backup_streamed,
        S.param4 AS tablespaces_total,
        S.param5 AS tablespaces_streamed
    FROM pg_stat_get_progress_info('BASEBACKUP') AS S;


CREATE VIEW pg_stat_progress_copy AS
    SELECT
        S.pid AS pid, S.datid AS datid, D.datname AS datname,
        S.relid AS relid,
        CASE S.param5 WHEN 1 THEN 'COPY FROM'
                      WHEN 2 THEN 'COPY TO'
                      END AS command,
        CASE S.param6 WHEN 1 THEN 'FILE'
                      WHEN 2 THEN 'PROGRAM'
                      WHEN 3 THEN 'PIPE'
                      WHEN 4 THEN 'CALLBACK'
                      END AS "type",
        S.param1 AS bytes_processed,
        S.param2 AS bytes_total,
        S.param3 AS tuples_processed,
        S.param4 AS tuples_excluded,
        S.param7 AS tuples_skipped
    FROM pg_stat_get_progress_info('COPY') AS S
        LEFT JOIN pg_database D ON S.datid = D.oid;

CREATE VIEW pg_user_mappings AS
    SELECT
        U.oid       AS umid,
        S.oid       AS srvid,
        S.srvname   AS srvname,
        U.umuser    AS umuser,
        CASE WHEN U.umuser = 0 THEN
            'public'
        ELSE
            A.rolname
        END AS usename,
        CASE WHEN (U.umuser <> 0 AND A.rolname = current_user
                     AND (pg_has_role(S.srvowner, 'USAGE')
                          OR has_server_privilege(S.oid, 'USAGE')))
                    OR (U.umuser = 0 AND pg_has_role(S.srvowner, 'USAGE'))
                    OR (SELECT rolsuper FROM pg_authid WHERE rolname = current_user)
                    THEN U.umoptions
                 ELSE NULL END AS umoptions
    FROM pg_user_mapping U
        JOIN pg_foreign_server S ON (U.umserver = S.oid)
        LEFT JOIN pg_authid A ON (A.oid = U.umuser);

REVOKE ALL ON pg_user_mapping FROM public;

CREATE VIEW pg_replication_origin_status AS
    SELECT *
    FROM pg_show_replication_origin_status();

REVOKE ALL ON pg_replication_origin_status FROM public;

-- All columns of pg_subscription except subconninfo are publicly readable.
REVOKE ALL ON pg_subscription FROM public;
GRANT SELECT (oid, subdbid, subskiplsn, subname, subowner, subenabled,
              subbinary, substream, subtwophasestate, subdisableonerr,
			  subpasswordrequired, subrunasowner, subfailover,
              subslotname, subsynccommit, subpublications, suborigin)
    ON pg_subscription TO public;

CREATE VIEW pg_stat_subscription_stats AS
    SELECT
        ss.subid,
        s.subname,
        ss.apply_error_count,
        ss.sync_error_count,
        ss.confl_insert_exists,
        ss.confl_update_origin_differs,
        ss.confl_update_exists,
        ss.confl_update_missing,
        ss.confl_delete_origin_differs,
        ss.confl_delete_missing,
        ss.confl_multiple_unique_conflicts,
        ss.stats_reset
    FROM pg_subscription as s,
         pg_stat_get_subscription_stats(s.oid) as ss;

CREATE VIEW pg_wait_events AS
    SELECT * FROM pg_get_wait_events();

CREATE VIEW pg_aios AS
    SELECT * FROM pg_get_aios();
REVOKE ALL ON pg_aios FROM PUBLIC;
GRANT SELECT ON pg_aios TO pg_read_all_stats;
REVOKE EXECUTE ON FUNCTION pg_get_aios() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_aios() TO pg_read_all_stats;
