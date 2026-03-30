/*
 * PostgreSQL System Functions
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/backend/catalog/system_functions.sql
 *
 * This file redefines certain built-in functions that are impractical
 * to fully define in pg_proc.dat.  In most cases that's because they use
 * SQL-standard function bodies and/or default expressions.  (But defaults
 * that are just constants can be entered in pg_proc.dat.)  The node
 * tree representations of those are too unreadable, platform-dependent,
 * and changeable to want to deal with them manually.  Hence, we put stub
 * definitions of such functions into pg_proc.dat and then replace them
 * here.  The stub definitions would be unnecessary were it not that we'd
 * like these functions to have stable OIDs, the same as other built-in
 * functions.  (That's important, for example, to their treatment by
 * postgres_fdw.)
 *
 * Note: this file is read in single-user -j mode, which means that the
 * command terminator is semicolon-newline-newline; whenever the backend
 * sees that, it stops and executes what it's got.  If you write a lot of
 * statements without empty lines between, they'll all get quoted to you
 * in any error message about one of them, so don't do that.  Also, you
 * cannot write a semicolon immediately followed by an empty line in a
 * string literal (including a function body!) or a multiline comment.
 */


CREATE OR REPLACE FUNCTION lpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN lpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION rpad(text, integer)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN rpad($1, $2, ' ');

CREATE OR REPLACE FUNCTION "substring"(text, text, text)
 RETURNS text
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN substring($1, similar_to_escape($2, $3));

CREATE OR REPLACE FUNCTION bit_length(bit)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN length($1);

CREATE OR REPLACE FUNCTION bit_length(bytea)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION bit_length(text)
 RETURNS integer
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN octet_length($1) * 8;

CREATE OR REPLACE FUNCTION log(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION log10(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN log(10, $1);

CREATE OR REPLACE FUNCTION round(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN round($1, 0);

CREATE OR REPLACE FUNCTION trunc(numeric)
 RETURNS numeric
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN trunc($1, 0);

CREATE OR REPLACE FUNCTION numeric_pl_pg_lsn(numeric, pg_lsn)
 RETURNS pg_lsn
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION path_contain_pt(path, point)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN on_ppath($2, $1);

CREATE OR REPLACE FUNCTION age(timestamptz)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamptz), $1);

CREATE OR REPLACE FUNCTION age(timestamp)
 RETURNS interval
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN age(cast(current_date as timestamp), $1);

CREATE OR REPLACE FUNCTION date_part(text, date)
 RETURNS double precision
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN date_part($1, cast($2 as timestamp));

CREATE OR REPLACE FUNCTION timestamptz(date, time)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN cast(($1 + $2) as timestamptz);

CREATE OR REPLACE FUNCTION timedate_pl(time, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION timetzdate_pl(timetz, date)
 RETURNS timestamptz
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_time(interval, time)
 RETURNS time
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_date(interval, date)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timetz(interval, timetz)
 RETURNS timetz
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamp(interval, timestamp)
 RETURNS timestamp
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION interval_pl_timestamptz(interval, timestamptz)
 RETURNS timestamptz
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION integer_pl_date(integer, date)
 RETURNS date
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, timestamptz,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, interval)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamptz, interval,
  timestamptz, timestamptz)
 RETURNS boolean
 LANGUAGE sql
 STABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, timestamp,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, timestamp)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION "overlaps"(timestamp, interval,
  timestamp, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, time,
  time, interval)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, $2) overlaps ($3, ($3 + $4));

CREATE OR REPLACE FUNCTION "overlaps"(time, interval,
  time, time)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE COST 1
RETURN ($1, ($1 + $2)) overlaps ($3, $4);

CREATE OR REPLACE FUNCTION int8pl_inet(bigint, inet)
 RETURNS inet
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN $2 + $1;

CREATE OR REPLACE FUNCTION xpath(text, xml)
 RETURNS xml[]
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN xpath($1, $2, '{}'::text[]);

CREATE OR REPLACE FUNCTION xpath_exists(text, xml)
 RETURNS boolean
 LANGUAGE sql
 IMMUTABLE PARALLEL SAFE STRICT COST 1
RETURN xpath_exists($1, $2, '{}'::text[]);

CREATE OR REPLACE FUNCTION pg_sleep_for(interval)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from clock_timestamp() + $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_sleep_until(timestamptz)
 RETURNS void
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_sleep(extract(epoch from $1) -
                extract(epoch from clock_timestamp()));

CREATE OR REPLACE FUNCTION pg_relation_size(regclass)
 RETURNS bigint
 LANGUAGE sql
 PARALLEL SAFE STRICT COST 1
RETURN pg_relation_size($1, 'main');

CREATE OR REPLACE FUNCTION obj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace) and
    objsubid = 0;
END;

CREATE OR REPLACE FUNCTION shobj_description(oid, name)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_shdescription
  where objoid = $1 and
    classoid = (select oid from pg_class where relname = $2 and
                relnamespace = 'pg_catalog'::regnamespace);
END;

CREATE OR REPLACE FUNCTION obj_description(oid)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description where objoid = $1 and objsubid = 0;
END;

CREATE OR REPLACE FUNCTION col_description(oid, integer)
 RETURNS text
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select description from pg_description
  where objoid = $1 and classoid = 'pg_class'::regclass and objsubid = $2;
END;

CREATE OR REPLACE FUNCTION ts_debug(config regconfig, document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
 RETURNS SETOF record
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
select
    tt.alias AS alias,
    tt.description AS description,
    parse.token AS token,
    ARRAY ( SELECT m.mapdict::regdictionary
            FROM pg_ts_config_map AS m
            WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
            ORDER BY m.mapseqno )
    AS dictionaries,
    ( SELECT mapdict::regdictionary
      FROM pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS dictionary,
    ( SELECT ts_lexize(mapdict, parse.token)
      FROM pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS lexemes
FROM ts_parse(
        (SELECT cfgparser FROM pg_ts_config WHERE oid = $1 ), $2
    ) AS parse,
     ts_token_type(
        (SELECT cfgparser FROM pg_ts_config WHERE oid = $1 )
    ) AS tt
WHERE tt.tokid = parse.tokid;
END;

CREATE OR REPLACE FUNCTION ts_debug(document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
 RETURNS SETOF record
 LANGUAGE sql
 STABLE PARALLEL SAFE STRICT
BEGIN ATOMIC
    SELECT * FROM ts_debug(get_current_ts_config(), $1);
END;
