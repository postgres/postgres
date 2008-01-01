/*
 * PostgreSQL System Views
 *
 * Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/catalog/system_views.sql,v 1.48 2008/01/01 19:45:48 momjian Exp $
 */

CREATE VIEW pg_roles AS 
    SELECT 
        rolname,
        rolsuper,
        rolinherit,
        rolcreaterole,
        rolcreatedb,
        rolcatupdate,
        rolcanlogin,
        rolconnlimit,
        '********'::text as rolpassword,
        rolvaliduntil,
        rolconfig,
        oid
    FROM pg_authid;

CREATE VIEW pg_shadow AS
    SELECT
        rolname AS usename,
        oid AS usesysid,
        rolcreatedb AS usecreatedb,
        rolsuper AS usesuper,
        rolcatupdate AS usecatupd,
        rolpassword AS passwd,
        rolvaliduntil::abstime AS valuntil,
        rolconfig AS useconfig
    FROM pg_authid
    WHERE rolcanlogin;

REVOKE ALL on pg_shadow FROM public;

CREATE VIEW pg_group AS
    SELECT
        rolname AS groname,
        oid AS grosysid,
        ARRAY(SELECT member FROM pg_auth_members WHERE roleid = oid) AS grolist
    FROM pg_authid
    WHERE NOT rolcanlogin;

CREATE VIEW pg_user AS 
    SELECT 
        usename, 
        usesysid, 
        usecreatedb, 
        usesuper, 
        usecatupd, 
        '********'::text as passwd, 
        valuntil, 
        useconfig 
    FROM pg_shadow;

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
        (C.reltriggers > 0) AS hastriggers 
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
         LEFT JOIN pg_tablespace T ON (T.oid = C.reltablespace)
    WHERE C.relkind = 'r';

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
    WHERE C.relkind = 'r' AND I.relkind = 'i';

CREATE VIEW pg_stats AS 
    SELECT 
        nspname AS schemaname, 
        relname AS tablename, 
        attname AS attname, 
        stanullfrac AS null_frac, 
        stawidth AS avg_width, 
        stadistinct AS n_distinct, 
        CASE 1 
            WHEN stakind1 THEN stavalues1 
            WHEN stakind2 THEN stavalues2 
            WHEN stakind3 THEN stavalues3 
            WHEN stakind4 THEN stavalues4 
        END AS most_common_vals, 
        CASE 1 
            WHEN stakind1 THEN stanumbers1 
            WHEN stakind2 THEN stanumbers2 
            WHEN stakind3 THEN stanumbers3 
            WHEN stakind4 THEN stanumbers4 
        END AS most_common_freqs, 
        CASE 2 
            WHEN stakind1 THEN stavalues1 
            WHEN stakind2 THEN stavalues2 
            WHEN stakind3 THEN stavalues3 
            WHEN stakind4 THEN stavalues4 
        END AS histogram_bounds, 
        CASE 3 
            WHEN stakind1 THEN stanumbers1[1] 
            WHEN stakind2 THEN stanumbers2[1] 
            WHEN stakind3 THEN stanumbers3[1] 
            WHEN stakind4 THEN stanumbers4[1] 
        END AS correlation 
    FROM pg_statistic s JOIN pg_class c ON (c.oid = s.starelid) 
         JOIN pg_attribute a ON (c.oid = attrelid AND attnum = s.staattnum) 
         LEFT JOIN pg_namespace n ON (n.oid = c.relnamespace) 
    WHERE has_table_privilege(c.oid, 'select');

REVOKE ALL on pg_statistic FROM public;

CREATE VIEW pg_locks AS 
    SELECT * 
    FROM pg_lock_status() AS L
    (locktype text, database oid, relation oid, page int4, tuple int2,
     virtualxid text, transactionid xid, classid oid, objid oid, objsubid int2,
     virtualtransaction text, pid int4, mode text, granted boolean);

CREATE VIEW pg_cursors AS
    SELECT C.name, C.statement, C.is_holdable, C.is_binary,
           C.is_scrollable, C.creation_time
    FROM pg_cursor() AS C
         (name text, statement text, is_holdable boolean, is_binary boolean,
          is_scrollable boolean, creation_time timestamptz);

CREATE VIEW pg_prepared_xacts AS
    SELECT P.transaction, P.gid, P.prepared,
           U.rolname AS owner, D.datname AS database
    FROM pg_prepared_xact() AS P
    (transaction xid, gid text, prepared timestamptz, ownerid oid, dbid oid)
         LEFT JOIN pg_authid U ON P.ownerid = U.oid
         LEFT JOIN pg_database D ON P.dbid = D.oid;

CREATE VIEW pg_prepared_statements AS
    SELECT P.name, P.statement, P.prepare_time, P.parameter_types, P.from_sql
    FROM pg_prepared_statement() AS P
    (name text, statement text, prepare_time timestamptz,
     parameter_types regtype[], from_sql boolean);

CREATE VIEW pg_settings AS 
    SELECT * 
    FROM pg_show_all_settings() AS A 
    (name text, setting text, unit text, category text, short_desc text, extra_desc text,
     context text, vartype text, source text, min_val text, max_val text);

CREATE RULE pg_settings_u AS 
    ON UPDATE TO pg_settings 
    WHERE new.name = old.name DO 
    SELECT set_config(old.name, new.setting, 'f');

CREATE RULE pg_settings_n AS 
    ON UPDATE TO pg_settings 
    DO INSTEAD NOTHING;

GRANT SELECT, UPDATE ON pg_settings TO PUBLIC;

CREATE VIEW pg_timezone_abbrevs AS
    SELECT * FROM pg_timezone_abbrevs();

CREATE VIEW pg_timezone_names AS
    SELECT * FROM pg_timezone_names();

-- Statistics views

CREATE VIEW pg_stat_all_tables AS 
    SELECT 
            C.oid AS relid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            pg_stat_get_numscans(C.oid) AS seq_scan, 
            pg_stat_get_tuples_returned(C.oid) AS seq_tup_read, 
            sum(pg_stat_get_numscans(I.indexrelid))::bigint AS idx_scan, 
            sum(pg_stat_get_tuples_fetched(I.indexrelid))::bigint +
            pg_stat_get_tuples_fetched(C.oid) AS idx_tup_fetch, 
            pg_stat_get_tuples_inserted(C.oid) AS n_tup_ins, 
            pg_stat_get_tuples_updated(C.oid) AS n_tup_upd, 
            pg_stat_get_tuples_deleted(C.oid) AS n_tup_del,
            pg_stat_get_tuples_hot_updated(C.oid) AS n_tup_hot_upd,
            pg_stat_get_live_tuples(C.oid) AS n_live_tup, 
            pg_stat_get_dead_tuples(C.oid) AS n_dead_tup,
            pg_stat_get_last_vacuum_time(C.oid) as last_vacuum,
            pg_stat_get_last_autovacuum_time(C.oid) as last_autovacuum,
            pg_stat_get_last_analyze_time(C.oid) as last_analyze,
            pg_stat_get_last_autoanalyze_time(C.oid) as last_autoanalyze
    FROM pg_class C LEFT JOIN 
         pg_index I ON C.oid = I.indrelid 
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't')
    GROUP BY C.oid, N.nspname, C.relname;

CREATE VIEW pg_stat_sys_tables AS 
    SELECT * FROM pg_stat_all_tables 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_user_tables AS 
    SELECT * FROM pg_stat_all_tables 
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
            sum(pg_stat_get_blocks_fetched(I.indexrelid) - 
                    pg_stat_get_blocks_hit(I.indexrelid))::bigint AS idx_blks_read, 
            sum(pg_stat_get_blocks_hit(I.indexrelid))::bigint AS idx_blks_hit, 
            pg_stat_get_blocks_fetched(T.oid) - 
                    pg_stat_get_blocks_hit(T.oid) AS toast_blks_read, 
            pg_stat_get_blocks_hit(T.oid) AS toast_blks_hit, 
            pg_stat_get_blocks_fetched(X.oid) - 
                    pg_stat_get_blocks_hit(X.oid) AS tidx_blks_read, 
            pg_stat_get_blocks_hit(X.oid) AS tidx_blks_hit 
    FROM pg_class C LEFT JOIN 
            pg_index I ON C.oid = I.indrelid LEFT JOIN 
            pg_class T ON C.reltoastrelid = T.oid LEFT JOIN 
            pg_class X ON T.reltoastidxid = X.oid 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't')
    GROUP BY C.oid, N.nspname, C.relname, T.oid, X.oid;

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
            pg_stat_get_tuples_returned(I.oid) AS idx_tup_read, 
            pg_stat_get_tuples_fetched(I.oid) AS idx_tup_fetch 
    FROM pg_class C JOIN 
            pg_index X ON C.oid = X.indrelid JOIN 
            pg_class I ON I.oid = X.indexrelid 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't');

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
    WHERE C.relkind IN ('r', 't');

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
            D.oid AS datid, 
            D.datname AS datname, 
            pg_stat_get_backend_pid(S.backendid) AS procpid, 
            pg_stat_get_backend_userid(S.backendid) AS usesysid, 
            U.rolname AS usename, 
            pg_stat_get_backend_activity(S.backendid) AS current_query,
            pg_stat_get_backend_waiting(S.backendid) AS waiting,
            pg_stat_get_backend_xact_start(S.backendid) AS xact_start,
            pg_stat_get_backend_activity_start(S.backendid) AS query_start,
            pg_stat_get_backend_start(S.backendid) AS backend_start,
            pg_stat_get_backend_client_addr(S.backendid) AS client_addr,
            pg_stat_get_backend_client_port(S.backendid) AS client_port
    FROM pg_database D, 
            (SELECT pg_stat_get_backend_idset() AS backendid) AS S, 
            pg_authid U 
    WHERE pg_stat_get_backend_dbid(S.backendid) = D.oid AND 
            pg_stat_get_backend_userid(S.backendid) = U.oid;

CREATE VIEW pg_stat_database AS 
    SELECT 
            D.oid AS datid, 
            D.datname AS datname, 
            pg_stat_get_db_numbackends(D.oid) AS numbackends, 
            pg_stat_get_db_xact_commit(D.oid) AS xact_commit, 
            pg_stat_get_db_xact_rollback(D.oid) AS xact_rollback, 
            pg_stat_get_db_blocks_fetched(D.oid) - 
                    pg_stat_get_db_blocks_hit(D.oid) AS blks_read, 
            pg_stat_get_db_blocks_hit(D.oid) AS blks_hit,
            pg_stat_get_db_tuples_returned(D.oid) AS tup_returned,
            pg_stat_get_db_tuples_fetched(D.oid) AS tup_fetched,
            pg_stat_get_db_tuples_inserted(D.oid) AS tup_inserted,
            pg_stat_get_db_tuples_updated(D.oid) AS tup_updated,
            pg_stat_get_db_tuples_deleted(D.oid) AS tup_deleted
    FROM pg_database D;

CREATE VIEW pg_stat_bgwriter AS
    SELECT
        pg_stat_get_bgwriter_timed_checkpoints() AS checkpoints_timed,
        pg_stat_get_bgwriter_requested_checkpoints() AS checkpoints_req,
        pg_stat_get_bgwriter_buf_written_checkpoints() AS buffers_checkpoint,
        pg_stat_get_bgwriter_buf_written_clean() AS buffers_clean,
        pg_stat_get_bgwriter_maxwritten_clean() AS maxwritten_clean,
        pg_stat_get_buf_written_backend() AS buffers_backend,
        pg_stat_get_buf_alloc() AS buffers_alloc;

-- Tsearch debug function.  Defined here because it'd be pretty unwieldy
-- to put it into pg_proc.h

CREATE FUNCTION ts_debug(IN config regconfig, IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
SELECT 
    tt.alias AS alias,
    tt.description AS description,
    parse.token AS token,
    ARRAY ( SELECT m.mapdict::pg_catalog.regdictionary
            FROM pg_catalog.pg_ts_config_map AS m
            WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
            ORDER BY m.mapseqno )
    AS dictionaries,
    ( SELECT mapdict::pg_catalog.regdictionary
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS dictionary,
    ( SELECT pg_catalog.ts_lexize(mapdict, parse.token)
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS lexemes
FROM pg_catalog.ts_parse(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 ), $2 
    ) AS parse,
     pg_catalog.ts_token_type(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 )
    ) AS tt
WHERE tt.tokid = parse.tokid
$$
LANGUAGE SQL STRICT STABLE;

COMMENT ON FUNCTION ts_debug(regconfig,text) IS
    'debug function for text search configuration';

CREATE FUNCTION ts_debug(IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
    SELECT * FROM pg_catalog.ts_debug( pg_catalog.get_current_ts_config(), $1);
$$
LANGUAGE SQL STRICT STABLE;

COMMENT ON FUNCTION ts_debug(text) IS
    'debug function for current text search configuration';
