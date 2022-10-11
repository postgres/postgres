CREATE SCHEMA IF NOT EXISTS _masking_function;
CREATE OR REPLACE FUNCTION _masking_function.default(in text, out text)
    AS $$ SELECT 'XXXX' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in real, out real)
    AS $$ SELECT 0 $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in date, out date)
    AS $$ SELECT DATE '1900-01-01' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in timestamp, out timestamp)
    AS $$ SELECT TIMESTAMP '1900-01-01 00:00:00' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in timestamptz, out timestamptz)
    AS $$ SELECT TIMESTAMPTZ '1900-01-01 00:00:00-00' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in time, out time)
    AS $$ SELECT TIME '00:00:00' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in timetz, out timetz)
    AS $$ SELECT TIMETZ '00:00:00-00' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in interval, out interval)
    AS $$ SELECT INTERVAL '1 year 2 months 3 days' $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in box, out box)
    AS $$ SELECT box(circle '((0,0),2.0)') $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in circle, out circle)
    AS $$ SELECT circle(point '(0,0)', 0) $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in path, out path)
    AS $$ SELECT '[ ( 0 , 1 ) , ( 1 , 2 ) ]'::path $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in point, out point)
    AS $$ SELECT '(0, 0)'::point $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in polygon , out polygon)
    AS $$ SELECT '( ( 0 , 0 ) , ( 0 , 0 ) )'::polygon $$
                     LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in bytea, out bytea)
    AS $$ SELECT '\000'::bytea $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in inet, out inet)
    AS $$ SELECT '0.0.0.0'::inet $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in cidr, out cidr)
    AS $$ SELECT '0.0.0.0'::cidr $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in macaddr, out macaddr)
    AS $$ SELECT macaddr '0:0:0:0:0:ab' $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in json, out json)
    AS $$ SELECT '{"a":"foo", "b":"bar"}'::json $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in jsonb, out jsonb)
    AS $$ SELECT '{"a":1, "b":2}'::jsonb $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in line, out line)
    AS $$ SELECT '{1,2,3}'::line $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in lseg, out lseg)
    AS $$ SELECT '((0,0),(0,0))'::lseg $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in bit, out bit)
    AS $$ SELECT '0'::bit $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in boolean, out boolean)
    AS $$ SELECT true $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in money, out money)
    AS $$ SELECT 0 $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in pg_lsn, out pg_lsn)
    AS $$ SELECT '0/0'::pg_lsn $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in uuid, out uuid)
    AS $$ SELECT '00000000-0000-0000-0000-000000000000'::uuid $$
                     LANGUAGE sql;

CREATE OR REPLACE FUNCTION _masking_function.default(in tsvector, out tsvector)
    AS $$ SELECT 'a:1'::tsvector $$
                     LANGUAGE sql;