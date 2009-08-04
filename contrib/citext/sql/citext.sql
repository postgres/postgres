--
--  Test citext datatype
--

--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of citext.sql.
--
SET client_min_messages = warning;
\set ECHO none
\i citext.sql
RESET client_min_messages;
\set ECHO all

-- Test the operators and indexing functions

-- Test = and <>.
SELECT 'a'::citext = 'a'::citext AS t;
SELECT 'a'::citext = 'A'::citext AS t;
SELECT 'a'::citext = 'A'::text AS f;        -- text wins the discussion
SELECT 'a'::citext = 'b'::citext AS f;
SELECT 'a'::citext = 'ab'::citext AS f;
SELECT 'a'::citext <> 'ab'::citext AS t;

-- Multibyte sanity tests. Uncomment to run.
-- SELECT 'À'::citext =  'À'::citext AS t;
-- SELECT 'À'::citext =  'à'::citext AS t;
-- SELECT 'À'::text   =  'à'::text   AS f; -- text wins.
-- SELECT 'À'::citext <> 'B'::citext AS t;

-- Test combining characters making up canonically equivalent strings.
-- SELECT 'Ä'::text   <> 'Ä'::text   AS t;
-- SELECT 'Ä'::citext <> 'Ä'::citext AS t;

-- Test the Turkish dotted I. The lowercase is a single byte while the
-- uppercase is multibyte. This is why the comparison code can't be optimized
-- to compare string lengths.
-- SELECT 'i'::citext = 'İ'::citext AS t;

-- Regression.
-- SELECT 'láska'::citext <> 'laská'::citext AS t;

-- SELECT 'Ask Bjørn Hansen'::citext = 'Ask Bjørn Hansen'::citext AS t;
-- SELECT 'Ask Bjørn Hansen'::citext = 'ASK BJØRN HANSEN'::citext AS t;
-- SELECT 'Ask Bjørn Hansen'::citext <> 'Ask Bjorn Hansen'::citext AS t;
-- SELECT 'Ask Bjørn Hansen'::citext <> 'ASK BJORN HANSEN'::citext AS t;
-- SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjørn Hansen'::citext) AS zero;
-- SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ask bjørn hansen'::citext) AS zero;
-- SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'ASK BJØRN HANSEN'::citext) AS zero;
-- SELECT citext_cmp('Ask Bjørn Hansen'::citext, 'Ask Bjorn Hansen'::citext) AS positive;
-- SELECT citext_cmp('Ask Bjorn Hansen'::citext, 'Ask Bjørn Hansen'::citext) AS negative;

-- Test > and >=
SELECT 'B'::citext > 'a'::citext AS t;
SELECT 'b'::citext >  'A'::citext AS t;
SELECT 'B'::citext >  'b'::citext AS f;
SELECT 'B'::citext >= 'b'::citext AS t;

-- Test < and <=
SELECT 'a'::citext <  'B'::citext AS t;
SELECT 'a'::citext <= 'B'::citext AS t;

-- Test implicit casting. citext casts to text, but not vice-versa.
SELECT 'a'::citext = 'a'::text   AS t;
SELECT 'A'::text  <> 'a'::citext AS t;

SELECT 'B'::citext <  'a'::text AS t;  -- text wins.
SELECT 'B'::citext <= 'a'::text AS t;  -- text wins.

SELECT 'a'::citext >  'B'::text AS t;  -- text wins.
SELECT 'a'::citext >= 'B'::text AS t;  -- text wins.

-- Test implicit casting. citext casts to varchar, but not vice-versa.
SELECT 'a'::citext = 'a'::varchar   AS t;
SELECT 'A'::varchar  <> 'a'::citext AS t;

SELECT 'B'::citext <  'a'::varchar AS t;  -- varchar wins.
SELECT 'B'::citext <= 'a'::varchar AS t;  -- varchar wins.

SELECT 'a'::citext >  'B'::varchar AS t;  -- varchar wins.
SELECT 'a'::citext >= 'B'::varchar AS t;  -- varchar wins.

-- A couple of longer examlpes to ensure that we don't get any issues with bad
-- conversions to char[] in the c code. Yes, I did do this.

SELECT 'aardvark'::citext = 'aardvark'::citext AS t;
SELECT 'aardvark'::citext = 'aardVark'::citext AS t;

-- Check the citext_cmp() function explicitly.
SELECT citext_cmp('aardvark'::citext, 'aardvark'::citext) AS zero;
SELECT citext_cmp('aardvark'::citext, 'aardVark'::citext) AS zero;
SELECT citext_cmp('AARDVARK'::citext, 'AARDVARK'::citext) AS zero;
SELECT citext_cmp('B'::citext, 'a'::citext) > 0 AS true;

-- Do some tests using a table and index.

CREATE TEMP TABLE try (
   name citext PRIMARY KEY
);

INSERT INTO try (name)
VALUES ('a'), ('ab'), ('â'), ('aba'), ('b'), ('ba'), ('bab'), ('AZ');

SELECT name, 'a' = name AS eq_a   FROM try WHERE name <> 'â';
SELECT name, 'a' = name AS t      FROM try where name = 'a';
SELECT name, 'A' = name AS "eq_A" FROM try WHERE name <> 'â';
SELECT name, 'A' = name AS t      FROM try where name = 'A';
SELECT name, 'A' = name AS t      FROM try where name = 'A';

-- expected failures on duplicate key
INSERT INTO try (name) VALUES ('a');
INSERT INTO try (name) VALUES ('A');
INSERT INTO try (name) VALUES ('aB');

-- Make sure that citext_smaller() and citext_lager() work properly.
SELECT citext_smaller( 'aa'::citext, 'ab'::citext ) = 'aa' AS t;
SELECT citext_smaller( 'AAAA'::citext, 'bbbb'::citext ) = 'AAAA' AS t;
SELECT citext_smaller( 'aardvark'::citext, 'Aaba'::citext ) = 'Aaba' AS t;
SELECT citext_smaller( 'aardvark'::citext, 'AARDVARK'::citext ) = 'AARDVARK' AS t;

SELECT citext_larger( 'aa'::citext, 'ab'::citext ) = 'ab' AS t;
SELECT citext_larger( 'AAAA'::citext, 'bbbb'::citext ) = 'bbbb' AS t;
SELECT citext_larger( 'aardvark'::citext, 'Aaba'::citext ) = 'aardvark' AS t;

-- Test aggregate functions and sort ordering

CREATE TEMP TABLE srt (
   name CITEXT
);

INSERT INTO srt (name)
VALUES ('aardvark'),
       ('AAA'),
       ('aba'),
       ('ABC'),
       ('abd');

-- Check the min() and max() aggregates, with and without index.
set enable_seqscan = off;
SELECT MIN(name) AS "AAA" FROM srt;
SELECT MAX(name) AS abd FROM srt;
reset enable_seqscan;
set enable_indexscan = off;
SELECT MIN(name) AS "AAA" FROM srt;
SELECT MAX(name) AS abd FROM srt;
reset enable_indexscan;

-- Check sorting likewise
set enable_seqscan = off;
SELECT name FROM srt ORDER BY name;
reset enable_seqscan;
set enable_indexscan = off;
SELECT name FROM srt ORDER BY name;
reset enable_indexscan;

-- Test assignment casts.
SELECT LOWER(name) as aaa FROM srt WHERE name = 'AAA'::text;
SELECT LOWER(name) as aaa FROM srt WHERE name = 'AAA'::varchar;
SELECT LOWER(name) as aaa FROM srt WHERE name = 'AAA'::bpchar;
SELECT LOWER(name) as aaa FROM srt WHERE name = 'AAA';
SELECT LOWER(name) as aaa FROM srt WHERE name = 'AAA'::citext;

-- LIKE should be case-insensitive
SELECT name FROM srt WHERE name     LIKE '%a%' ORDER BY name;
SELECT name FROM srt WHERE name NOT LIKE '%b%' ORDER BY name;
SELECT name FROM srt WHERE name     LIKE '%A%' ORDER BY name;
SELECT name FROM srt WHERE name NOT LIKE '%B%' ORDER BY name;

-- ~~ should be case-insensitive
SELECT name FROM srt WHERE name ~~  '%a%' ORDER BY name;
SELECT name FROM srt WHERE name !~~ '%b%' ORDER BY name;
SELECT name FROM srt WHERE name ~~  '%A%' ORDER BY name;
SELECT name FROM srt WHERE name !~~ '%B%' ORDER BY name;

-- ~ should be case-insensitive
SELECT name FROM srt WHERE name ~  '^a' ORDER BY name;
SELECT name FROM srt WHERE name !~ 'a$' ORDER BY name;
SELECT name FROM srt WHERE name ~  '^A' ORDER BY name;
SELECT name FROM srt WHERE name !~ 'A$' ORDER BY name;

-- SIMILAR TO should be case-insensitive.
SELECT name FROM srt WHERE name SIMILAR TO '%a.*';
SELECT name FROM srt WHERE name SIMILAR TO '%A.*';

-- Explicit casts.
SELECT true::citext = 'true' AS t;
SELECT 'true'::citext::boolean = true AS t;

SELECT 4::citext = '4' AS t;
SELECT 4::int4::citext = '4' AS t;
SELECT '4'::citext::int4 = 4 AS t;
SELECT 4::integer::citext = '4' AS t;
SELECT '4'::citext::integer = 4 AS t;

SELECT 4::int8::citext = '4' AS t;
SELECT '4'::citext::int8 = 4 AS t;
SELECT 4::bigint::citext = '4' AS t;
SELECT '4'::citext::bigint = 4 AS t;

SELECT 4::int2::citext = '4' AS t;
SELECT '4'::citext::int2 = 4 AS t;
SELECT 4::smallint::citext = '4' AS t;
SELECT '4'::citext::smallint = 4 AS t;

SELECT 4.0::numeric = '4.0' AS t;
SELECT '4.0'::citext::numeric = 4.0 AS t;
SELECT 4.0::decimal = '4.0' AS t;
SELECT '4.0'::citext::decimal = 4.0 AS t;

SELECT 4.0::real = '4.0' AS t;
SELECT '4.0'::citext::real = 4.0 AS t;
SELECT 4.0::float4 = '4.0' AS t;
SELECT '4.0'::citext::float4 = 4.0 AS t;

SELECT 4.0::double precision = '4.0' AS t;
SELECT '4.0'::citext::double precision = 4.0 AS t;
SELECT 4.0::float8 = '4.0' AS t;
SELECT '4.0'::citext::float8 = 4.0 AS t;

SELECT 'foo'::name::citext = 'foo' AS t;
SELECT 'foo'::citext::name = 'foo'::name AS t;

SELECT 'f'::char::citext = 'f' AS t;
SELECT 'f'::citext::char = 'f'::char AS t;

SELECT 'f'::"char"::citext = 'f' AS t;
SELECT 'f'::citext::"char" = 'f'::"char" AS t;

SELECT '100'::money::citext = '$100.00' AS t;
SELECT '100'::citext::money = '100'::money AS t;

SELECT 'a'::char::citext = 'a' AS t;
SELECT 'a'::citext::char = 'a'::char AS t;

SELECT 'foo'::varchar::citext = 'foo' AS t;
SELECT 'foo'::citext::varchar = 'foo'::varchar AS t;

SELECT 'foo'::text::citext = 'foo' AS t;
SELECT 'foo'::citext::text = 'foo'::text AS t;

SELECT '192.168.100.128/25'::cidr::citext = '192.168.100.128/25' AS t;
SELECT '192.168.100.128/25'::citext::cidr = '192.168.100.128/25'::cidr AS t;

SELECT '192.168.100.128'::inet::citext = '192.168.100.128/32' AS t;
SELECT '192.168.100.128'::citext::inet = '192.168.100.128'::inet AS t;

SELECT '08:00:2b:01:02:03'::macaddr::citext = '08:00:2b:01:02:03' AS t;
SELECT '08:00:2b:01:02:03'::citext::macaddr = '08:00:2b:01:02:03'::macaddr AS t;

SELECT '1999-01-08 04:05:06'::timestamp::citext = '1999-01-08 04:05:06'::timestamp::text AS t;
SELECT '1999-01-08 04:05:06'::citext::timestamp = '1999-01-08 04:05:06'::timestamp AS t;
SELECT '1999-01-08 04:05:06'::timestamptz::citext = '1999-01-08 04:05:06'::timestamptz::text AS t;
SELECT '1999-01-08 04:05:06'::citext::timestamptz = '1999-01-08 04:05:06'::timestamptz AS t;

SELECT '1 hour'::interval::citext = '1 hour'::interval::text AS t;
SELECT '1 hour'::citext::interval = '1 hour'::interval AS t;

SELECT '1999-01-08'::date::citext = '1999-01-08'::date::text AS t;
SELECT '1999-01-08'::citext::date = '1999-01-08'::date AS t;

SELECT '04:05:06'::time::citext = '04:05:06' AS t;
SELECT '04:05:06'::citext::time = '04:05:06'::time AS t;
SELECT '04:05:06'::timetz::citext = '04:05:06'::timetz::text AS t;
SELECT '04:05:06'::citext::timetz = '04:05:06'::timetz AS t;

SELECT '( 1 , 1)'::point::citext = '(1,1)' AS t;
SELECT '( 1 , 1)'::citext::point ~= '(1,1)'::point AS t;
SELECT '( 1 , 1 ) , ( 2 , 2 )'::lseg::citext = '[(1,1),(2,2)]' AS t;
SELECT '( 1 , 1 ) , ( 2 , 2 )'::citext::lseg = '[(1,1),(2,2)]'::lseg AS t;
SELECT '( 0 , 0 ) , ( 1 , 1 )'::box::citext = '(0,0),(1,1)'::box::text AS t;
SELECT '( 0 , 0 ) , ( 1 , 1 )'::citext::box ~= '(0,0),(1,1)'::text::box AS t;

SELECT '((0,0),(1,1),(2,0))'::path::citext = '((0,0),(1,1),(2,0))' AS t;
SELECT '((0,0),(1,1),(2,0))'::citext::path = '((0,0),(1,1),(2,0))'::path AS t;

SELECT '((0,0),(1,1))'::polygon::citext = '((0,0),(1,1))' AS t;
SELECT '((0,0),(1,1))'::citext::polygon ~= '((0,0),(1,1))'::polygon AS t;

SELECT '((0,0),2)'::circle::citext = '((0,0),2)'::circle::text AS t;
SELECT '((0,0),2)'::citext::circle ~= '((0,0),2)'::text::circle AS t;

SELECT '101'::bit::citext = '101'::bit::text AS t;
SELECT '101'::citext::bit = '101'::text::bit AS t;
SELECT '101'::bit varying::citext = '101'::bit varying::text AS t;
SELECT '101'::citext::bit varying = '101'::text::bit varying AS t;
SELECT 'a fat cat'::tsvector::citext = '''a'' ''cat'' ''fat''' AS t;
SELECT 'a fat cat'::citext::tsvector = 'a fat cat'::tsvector AS t;
SELECT 'fat & rat'::tsquery::citext = '''fat'' & ''rat''' AS t;
SELECT 'fat & rat'::citext::tsquery = 'fat & rat'::tsquery AS t;
SELECT 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid::citext = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' AS t;
SELECT 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::citext::uuid = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid AS t;

CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
SELECT 'sad'::mood::citext = 'sad' AS t;
SELECT 'sad'::citext::mood = 'sad'::mood AS t;

-- Assignment casts.
CREATE TABLE caster (
    citext      citext,
    text        text,
    varchar     varchar,
    bpchar      bpchar,
    char        char,
    chr         "char",
    name        name,    
    bytea       bytea,
    boolean     boolean,
    float4      float4,
    float8      float8,
    numeric     numeric,
    int8        int8,
    int4        int4,
    int2        int2,
    cidr        cidr,   
    inet        inet,
    macaddr     macaddr,
    money       money,
    timestamp   timestamp,
    timestamptz timestamptz,
    interval    interval,
    date        date,
    time        time,
    timetz      timetz,
    point       point,
    lseg        lseg,
    box         box,
    path        path,
    polygon     polygon,
    circle      circle,
    bit         bit,
    bitv        bit varying,
    tsvector    tsvector,
    tsquery     tsquery,
    uuid        uuid
);

INSERT INTO caster (text)          VALUES ('foo'::citext);
INSERT INTO caster (citext)        VALUES ('foo'::text);

INSERT INTO caster (varchar)       VALUES ('foo'::text);
INSERT INTO caster (text)          VALUES ('foo'::varchar);
INSERT INTO caster (varchar)       VALUES ('foo'::citext);
INSERT INTO caster (citext)        VALUES ('foo'::varchar);

INSERT INTO caster (bpchar)        VALUES ('foo'::text);
INSERT INTO caster (text)          VALUES ('foo'::bpchar);
INSERT INTO caster (bpchar)        VALUES ('foo'::citext);
INSERT INTO caster (citext)        VALUES ('foo'::bpchar);

INSERT INTO caster (char)          VALUES ('f'::text);
INSERT INTO caster (text)          VALUES ('f'::char);
INSERT INTO caster (char)          VALUES ('f'::citext);
INSERT INTO caster (citext)        VALUES ('f'::char);

INSERT INTO caster (chr)           VALUES ('f'::text);
INSERT INTO caster (text)          VALUES ('f'::"char");
INSERT INTO caster (chr)           VALUES ('f'::citext);
INSERT INTO caster (citext)        VALUES ('f'::"char");

INSERT INTO caster (name)          VALUES ('foo'::text);
INSERT INTO caster (text)          VALUES ('foo'::name);
INSERT INTO caster (name)          VALUES ('foo'::citext);
INSERT INTO caster (citext)        VALUES ('foo'::name);

-- Cannot cast to bytea on assignment.
INSERT INTO caster (bytea)         VALUES ('foo'::text);
INSERT INTO caster (text)          VALUES ('foo'::bytea);
INSERT INTO caster (bytea)         VALUES ('foo'::citext);
INSERT INTO caster (citext)        VALUES ('foo'::bytea);

-- Cannot cast to boolean on assignment.
INSERT INTO caster (boolean)       VALUES ('t'::text);
INSERT INTO caster (text)          VALUES ('t'::boolean);
INSERT INTO caster (boolean)       VALUES ('t'::citext);
INSERT INTO caster (citext)        VALUES ('t'::boolean);

-- Cannot cast to float8 on assignment.
INSERT INTO caster (float8)        VALUES ('12.42'::text);
INSERT INTO caster (text)          VALUES ('12.42'::float8);
INSERT INTO caster (float8)        VALUES ('12.42'::citext);
INSERT INTO caster (citext)        VALUES ('12.42'::float8);

-- Cannot cast to float4 on assignment.
INSERT INTO caster (float4)        VALUES ('12.42'::text);
INSERT INTO caster (text)          VALUES ('12.42'::float4);
INSERT INTO caster (float4)        VALUES ('12.42'::citext);
INSERT INTO caster (citext)        VALUES ('12.42'::float4);

-- Cannot cast to numeric on assignment.
INSERT INTO caster (numeric)       VALUES ('12.42'::text);
INSERT INTO caster (text)          VALUES ('12.42'::numeric);
INSERT INTO caster (numeric)       VALUES ('12.42'::citext);
INSERT INTO caster (citext)        VALUES ('12.42'::numeric);

-- Cannot cast to int8 on assignment.
INSERT INTO caster (int8)          VALUES ('12'::text);
INSERT INTO caster (text)          VALUES ('12'::int8);
INSERT INTO caster (int8)          VALUES ('12'::citext);
INSERT INTO caster (citext)        VALUES ('12'::int8);

-- Cannot cast to int4 on assignment.
INSERT INTO caster (int4)          VALUES ('12'::text);
INSERT INTO caster (text)          VALUES ('12'::int4);
INSERT INTO caster (int4)          VALUES ('12'::citext);
INSERT INTO caster (citext)        VALUES ('12'::int4);

-- Cannot cast to int2 on assignment.
INSERT INTO caster (int2)          VALUES ('12'::text);
INSERT INTO caster (text)          VALUES ('12'::int2);
INSERT INTO caster (int2)          VALUES ('12'::citext);
INSERT INTO caster (citext)        VALUES ('12'::int2);

-- Cannot cast to cidr on assignment.
INSERT INTO caster (cidr)          VALUES ('192.168.100.128/25'::text);
INSERT INTO caster (text)          VALUES ('192.168.100.128/25'::cidr);
INSERT INTO caster (cidr)          VALUES ('192.168.100.128/25'::citext);
INSERT INTO caster (citext)        VALUES ('192.168.100.128/25'::cidr);

-- Cannot cast to inet on assignment.
INSERT INTO caster (inet)          VALUES ('192.168.100.128'::text);
INSERT INTO caster (text)          VALUES ('192.168.100.128'::inet);
INSERT INTO caster (inet)          VALUES ('192.168.100.128'::citext);
INSERT INTO caster (citext)        VALUES ('192.168.100.128'::inet);

-- Cannot cast to macaddr on assignment.
INSERT INTO caster (macaddr)       VALUES ('08:00:2b:01:02:03'::text);
INSERT INTO caster (text)          VALUES ('08:00:2b:01:02:03'::macaddr);
INSERT INTO caster (macaddr)       VALUES ('08:00:2b:01:02:03'::citext);
INSERT INTO caster (citext)        VALUES ('08:00:2b:01:02:03'::macaddr);

-- Cannot cast to money on assignment.
INSERT INTO caster (money)         VALUES ('12'::text);
INSERT INTO caster (text)          VALUES ('12'::money);
INSERT INTO caster (money)         VALUES ('12'::citext);
INSERT INTO caster (citext)        VALUES ('12'::money);

-- Cannot cast to timestamp on assignment.
INSERT INTO caster (timestamp)     VALUES ('1999-01-08 04:05:06'::text);
INSERT INTO caster (text)          VALUES ('1999-01-08 04:05:06'::timestamp);
INSERT INTO caster (timestamp)     VALUES ('1999-01-08 04:05:06'::citext);
INSERT INTO caster (citext)        VALUES ('1999-01-08 04:05:06'::timestamp);

-- Cannot cast to timestamptz on assignment.
INSERT INTO caster (timestamptz)   VALUES ('1999-01-08 04:05:06'::text);
INSERT INTO caster (text)          VALUES ('1999-01-08 04:05:06'::timestamptz);
INSERT INTO caster (timestamptz)   VALUES ('1999-01-08 04:05:06'::citext);
INSERT INTO caster (citext)        VALUES ('1999-01-08 04:05:06'::timestamptz);

-- Cannot cast to interval on assignment.
INSERT INTO caster (interval)      VALUES ('1 hour'::text);
INSERT INTO caster (text)          VALUES ('1 hour'::interval);
INSERT INTO caster (interval)      VALUES ('1 hour'::citext);
INSERT INTO caster (citext)        VALUES ('1 hour'::interval);

-- Cannot cast to date on assignment.
INSERT INTO caster (date)          VALUES ('1999-01-08'::text);
INSERT INTO caster (text)          VALUES ('1999-01-08'::date);
INSERT INTO caster (date)          VALUES ('1999-01-08'::citext);
INSERT INTO caster (citext)        VALUES ('1999-01-08'::date);

-- Cannot cast to time on assignment.
INSERT INTO caster (time)          VALUES ('04:05:06'::text);
INSERT INTO caster (text)          VALUES ('04:05:06'::time);
INSERT INTO caster (time)          VALUES ('04:05:06'::citext);
INSERT INTO caster (citext)        VALUES ('04:05:06'::time);

-- Cannot cast to timetz on assignment.
INSERT INTO caster (timetz)        VALUES ('04:05:06'::text);
INSERT INTO caster (text)          VALUES ('04:05:06'::timetz);
INSERT INTO caster (timetz)        VALUES ('04:05:06'::citext);
INSERT INTO caster (citext)        VALUES ('04:05:06'::timetz);

-- Cannot cast to point on assignment.
INSERT INTO caster (point)         VALUES ('( 1 , 1)'::text);
INSERT INTO caster (text)          VALUES ('( 1 , 1)'::point);
INSERT INTO caster (point)         VALUES ('( 1 , 1)'::citext);
INSERT INTO caster (citext)        VALUES ('( 1 , 1)'::point);

-- Cannot cast to lseg on assignment.
INSERT INTO caster (lseg)          VALUES ('( 1 , 1 ) , ( 2 , 2 )'::text);
INSERT INTO caster (text)          VALUES ('( 1 , 1 ) , ( 2 , 2 )'::lseg);
INSERT INTO caster (lseg)          VALUES ('( 1 , 1 ) , ( 2 , 2 )'::citext);
INSERT INTO caster (citext)        VALUES ('( 1 , 1 ) , ( 2 , 2 )'::lseg);

-- Cannot cast to box on assignment.
INSERT INTO caster (box)           VALUES ('(0,0),(1,1)'::text);
INSERT INTO caster (text)          VALUES ('(0,0),(1,1)'::box);
INSERT INTO caster (box)           VALUES ('(0,0),(1,1)'::citext);
INSERT INTO caster (citext)        VALUES ('(0,0),(1,1)'::box);

-- Cannot cast to path on assignment.
INSERT INTO caster (path)          VALUES ('((0,0),(1,1),(2,0))'::text);
INSERT INTO caster (text)          VALUES ('((0,0),(1,1),(2,0))'::path);
INSERT INTO caster (path)          VALUES ('((0,0),(1,1),(2,0))'::citext);
INSERT INTO caster (citext)        VALUES ('((0,0),(1,1),(2,0))'::path);

-- Cannot cast to polygon on assignment.
INSERT INTO caster (polygon)       VALUES ('((0,0),(1,1))'::text);
INSERT INTO caster (text)          VALUES ('((0,0),(1,1))'::polygon);
INSERT INTO caster (polygon)       VALUES ('((0,0),(1,1))'::citext);
INSERT INTO caster (citext)        VALUES ('((0,0),(1,1))'::polygon);

-- Cannot cast to circle on assignment.
INSERT INTO caster (circle)        VALUES ('((0,0),2)'::text);
INSERT INTO caster (text)          VALUES ('((0,0),2)'::circle);
INSERT INTO caster (circle)        VALUES ('((0,0),2)'::citext);
INSERT INTO caster (citext)        VALUES ('((0,0),2)'::circle);

-- Cannot cast to bit on assignment.
INSERT INTO caster (bit)           VALUES ('101'::text);
INSERT INTO caster (text)          VALUES ('101'::bit);
INSERT INTO caster (bit)           VALUES ('101'::citext);
INSERT INTO caster (citext)        VALUES ('101'::bit);

-- Cannot cast to bit varying on assignment.
INSERT INTO caster (bitv)          VALUES ('101'::text);
INSERT INTO caster (text)          VALUES ('101'::bit varying);
INSERT INTO caster (bitv)          VALUES ('101'::citext);
INSERT INTO caster (citext)        VALUES ('101'::bit varying);

-- Cannot cast to tsvector on assignment.
INSERT INTO caster (tsvector)      VALUES ('the fat cat'::text);
INSERT INTO caster (text)          VALUES ('the fat cat'::tsvector);
INSERT INTO caster (tsvector)      VALUES ('the fat cat'::citext);
INSERT INTO caster (citext)        VALUES ('the fat cat'::tsvector);

-- Cannot cast to tsquery on assignment.
INSERT INTO caster (tsquery)       VALUES ('fat & rat'::text);
INSERT INTO caster (text)          VALUES ('fat & rat'::tsquery);
INSERT INTO caster (tsquery)       VALUES ('fat & rat'::citext);
INSERT INTO caster (citext)        VALUES ('fat & rat'::tsquery);

-- Cannot cast to uuid on assignment.
INSERT INTO caster (uuid)          VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::text);
INSERT INTO caster (text)          VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid);
INSERT INTO caster (uuid)          VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::citext);
INSERT INTO caster (citext)        VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid);

-- Table 9-5. SQL String Functions and Operators
SELECT 'D'::citext || 'avid'::citext = 'David'::citext AS citext_concat;
SELECT 'Value: '::citext || 42 = 'Value: 42' AS text_concat;
SELECT  42 || ': value'::citext ='42: value' AS int_concat;
SELECT bit_length('jose'::citext) = 32 AS t;
SELECT bit_length( name ) = bit_length( name::text ) AS t FROM srt;
SELECT textlen( name ) = textlen( name::text ) AS t FROM srt;
SELECT char_length( name ) = char_length( name::text ) AS t FROM srt;
SELECT lower( name ) = lower( name::text ) AS t FROM srt;
SELECT octet_length( name ) = octet_length( name::text ) AS t FROM srt;
SELECT overlay( name placing 'hom' from 2 for 4) = overlay( name::text placing 'hom' from 2 for 4) AS t FROM srt;
SELECT position( 'a' IN name ) = position( 'a' IN name::text ) AS t FROM srt;

SELECT substr('alphabet'::citext, 3)       = 'phabet' AS t;
SELECT substr('alphabet'::citext, 3, 2)    = 'ph' AS t;

SELECT substring('alphabet'::citext, 3)    = 'phabet' AS t;
SELECT substring('alphabet'::citext, 3, 2) = 'ph' AS t;
SELECT substring('Thomas'::citext from 2 for 3) = 'hom' AS t;
SELECT substring('Thomas'::citext from 2) = 'homas' AS t;
SELECT substring('Thomas'::citext from '...$') = 'mas' AS t;
SELECT substring('Thomas'::citext from '%#"o_a#"_' for '#') = 'oma' AS t;

SELECT trim('    trim    '::citext)               = 'trim' AS t;
SELECT trim('xxxxxtrimxxxx'::citext, 'x'::citext) = 'trim' AS t;
SELECT trim('xxxxxxtrimxxxx'::text,  'x'::citext) = 'trim' AS t;
SELECT trim('xxxxxtrimxxxx'::text,   'x'::citext) = 'trim' AS t;

SELECT upper( name ) = upper( name::text ) AS t FROM srt;

-- Table 9-6. Other String Functions.
SELECT ascii( name ) = ascii( name::text ) AS t FROM srt;

SELECT btrim('    trim'::citext                   ) = 'trim' AS t;
SELECT btrim('xxxxxtrimxxxx'::citext, 'x'::citext ) = 'trim' AS t;
SELECT btrim('xyxtrimyyx'::citext,    'xy'::citext) = 'trim' AS t;
SELECT btrim('xyxtrimyyx'::text,      'xy'::citext) = 'trim' AS t;
SELECT btrim('xyxtrimyyx'::citext,    'xy'::text  ) = 'trim' AS t;

-- chr() takes an int and returns text.
-- convert() and convert_from take bytea and return text.

SELECT convert_to( name, 'ISO-8859-1' ) = convert_to( name::text, 'ISO-8859-1' ) AS t FROM srt;
SELECT decode('MTIzAAE='::citext, 'base64') = decode('MTIzAAE='::text, 'base64') AS t;
-- encode() takes bytea and returns text.
SELECT initcap('hi THOMAS'::citext) = initcap('hi THOMAS'::text) AS t;
SELECT length( name ) = length( name::text ) AS t FROM srt;

SELECT lpad('hi'::citext, 5              ) = '   hi' AS t;
SELECT lpad('hi'::citext, 5, 'xy'::citext) = 'xyxhi' AS t;
SELECT lpad('hi'::text,   5, 'xy'::citext) = 'xyxhi' AS t;
SELECT lpad('hi'::citext, 5, 'xy'::text  ) = 'xyxhi' AS t;

SELECT ltrim('    trim'::citext               ) = 'trim' AS t;
SELECT ltrim('zzzytrim'::citext, 'xyz'::citext) = 'trim' AS t;
SELECT ltrim('zzzytrim'::text,   'xyz'::citext) = 'trim' AS t;
SELECT ltrim('zzzytrim'::citext, 'xyz'::text  ) = 'trim' AS t;

SELECT md5( name ) = md5( name::text ) AS t FROM srt;
-- pg_client_encoding() takes no args and returns name.
SELECT quote_ident( name ) = quote_ident( name::text ) AS t FROM srt;
SELECT quote_literal( name ) = quote_literal( name::text ) AS t FROM srt;

SELECT regexp_matches('foobarbequebaz'::citext, '(bar)(beque)') = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)') = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)'::citext) = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)'::citext, '') = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)', '') = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz', '(BAR)(BEQUE)'::citext, '') = ARRAY[ 'bar', 'beque' ] AS t;
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)'::citext, ''::citext) = ARRAY[ 'bar', 'beque' ] AS t;
-- c forces case-sensitive
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)'::citext, 'c'::citext) = ARRAY[ 'bar', 'beque' ] AS "null";

SELECT regexp_replace('Thomas'::citext, '.[mN]a.',         'M') = 'ThM' AS t;
SELECT regexp_replace('Thomas'::citext, '.[MN]A.',         'M') = 'ThM' AS t;
SELECT regexp_replace('Thomas',         '.[MN]A.'::citext, 'M') = 'ThM' AS t;
SELECT regexp_replace('Thomas'::citext, '.[MN]A.'::citext, 'M') = 'ThM' AS t;
-- c forces case-sensitive
SELECT regexp_replace('Thomas'::citext, '.[MN]A.'::citext, 'M', 'c') = 'Thomas' AS t;

SELECT regexp_split_to_array('hello world'::citext, E'\\s+') = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld'::citext, 't') = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld', 't'::citext) = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld'::citext, 't'::citext) = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld'::citext, 't', 's') = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld', 't'::citext, 's') = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_array('helloTworld'::citext, 't'::citext, 's') = ARRAY[ 'hello', 'world' ] AS t;

-- c forces case-sensitive
SELECT regexp_split_to_array('helloTworld'::citext, 't'::citext, 'c') = ARRAY[ 'helloTworld' ] AS t;

SELECT regexp_split_to_table('hello world'::citext, E'\\s+') AS words;
SELECT regexp_split_to_table('helloTworld'::citext, 't') AS words;
SELECT regexp_split_to_table('helloTworld',         't'::citext) AS words;
SELECT regexp_split_to_table('helloTworld'::citext, 't'::citext) AS words;
-- c forces case-sensitive
SELECT regexp_split_to_table('helloTworld'::citext, 't'::citext, 'c') AS word;

SELECT repeat('Pg'::citext, 4) = 'PgPgPgPg' AS t;

SELECT replace('abcdefabcdef'::citext, 'cd', 'XX') = 'abXXefabXXef' AS t;
SELECT replace('abcdefabcdef'::citext, 'CD', 'XX') = 'abXXefabXXef' AS t;
SELECT replace('ab^is$abcdef'::citext, '^is$', 'XX') = 'abXXabcdef' AS t;
SELECT replace('abcdefabcdef', 'cd'::citext, 'XX') = 'abXXefabXXef' AS t;
SELECT replace('abcdefabcdef', 'CD'::citext, 'XX') = 'abXXefabXXef' AS t;
SELECT replace('ab^is$abcdef', '^is$'::citext, 'XX') = 'abXXabcdef' AS t;
SELECT replace('abcdefabcdef'::citext, 'cd'::citext, 'XX') = 'abXXefabXXef' AS t;
SELECT replace('abcdefabcdef'::citext, 'CD'::citext, 'XX') = 'abXXefabXXef' AS t;
SELECT replace('ab^is$abcdef'::citext, '^is$'::citext, 'XX') = 'abXXabcdef' AS t;

SELECT rpad('hi'::citext, 5              ) = 'hi   ' AS t;
SELECT rpad('hi'::citext, 5, 'xy'::citext) = 'hixyx' AS t;
SELECT rpad('hi'::text,   5, 'xy'::citext) = 'hixyx' AS t;
SELECT rpad('hi'::citext, 5, 'xy'::text  ) = 'hixyx' AS t;

SELECT rtrim('trim    '::citext             ) = 'trim' AS t;
SELECT rtrim('trimxxxx'::citext, 'x'::citext) = 'trim' AS t;
SELECT rtrim('trimxxxx'::text,   'x'::citext) = 'trim' AS t;
SELECT rtrim('trimxxxx'::text,   'x'::text  ) = 'trim' AS t;

SELECT split_part('abc~@~def~@~ghi'::citext, '~@~', 2) = 'def' AS t;
SELECT split_part('abcTdefTghi'::citext, 't', 2) = 'def' AS t;
SELECT split_part('abcTdefTghi'::citext, 't'::citext, 2) = 'def' AS t;
SELECT split_part('abcTdefTghi', 't'::citext, 2) = 'def' AS t;

SELECT strpos('high'::citext, 'ig'        ) = 2 AS t;
SELECT strpos('high',         'ig'::citext) = 2 AS t;
SELECT strpos('high'::citext, 'ig'::citext) = 2 AS t;
SELECT strpos('high'::citext, 'IG'        ) = 2 AS t;
SELECT strpos('high',         'IG'::citext) = 2 AS t;
SELECT strpos('high'::citext, 'IG'::citext) = 2 AS t;

-- to_ascii() does not support UTF-8.
-- to_hex() takes a numeric argument.
SELECT substr('alphabet', 3, 2) = 'ph' AS t;
SELECT translate('abcdefabcdef'::citext, 'cd',         'XX') = 'abXXefabXXef' AS t;
SELECT translate('abcdefabcdef'::citext, 'CD',         'XX') = 'abXXefabXXef' AS t;
SELECT translate('abcdefabcdef'::citext, 'CD'::citext, 'XX') = 'abXXefabXXef' AS t;
SELECT translate('abcdefabcdef',         'CD'::citext, 'XX') = 'abXXefabXXef' AS t;

-- Table 9-20. Formatting Functions
SELECT to_date('05 Dec 2000'::citext, 'DD Mon YYYY'::citext)
     = to_date('05 Dec 2000',         'DD Mon YYYY') AS t;
SELECT to_date('05 Dec 2000'::citext, 'DD Mon YYYY')
     = to_date('05 Dec 2000',         'DD Mon YYYY') AS t;
SELECT to_date('05 Dec 2000',         'DD Mon YYYY'::citext)
     = to_date('05 Dec 2000',         'DD Mon YYYY') AS t;

SELECT to_number('12,454.8-'::citext, '99G999D9S'::citext)
     = to_number('12,454.8-',         '99G999D9S') AS t;
SELECT to_number('12,454.8-'::citext, '99G999D9S')
     = to_number('12,454.8-',         '99G999D9S') AS t;
SELECT to_number('12,454.8-',         '99G999D9S'::citext)
     = to_number('12,454.8-',         '99G999D9S') AS t;

SELECT to_timestamp('05 Dec 2000'::citext, 'DD Mon YYYY'::citext)
     = to_timestamp('05 Dec 2000',         'DD Mon YYYY') AS t;
SELECT to_timestamp('05 Dec 2000'::citext, 'DD Mon YYYY')
     = to_timestamp('05 Dec 2000',         'DD Mon YYYY') AS t;
SELECT to_timestamp('05 Dec 2000',         'DD Mon YYYY'::citext)
     = to_timestamp('05 Dec 2000',         'DD Mon YYYY') AS t;

-- Try assigning function results to a column.
SELECT COUNT(*) = 8::bigint AS t FROM try;
INSERT INTO try
VALUES ( to_char(  now()::timestamp,          'HH12:MI:SS') ),
       ( to_char(  now() + '1 sec'::interval, 'HH12:MI:SS') ), -- timetamptz
       ( to_char(  '15h 2m 12s'::interval,    'HH24:MI:SS') ),
       ( to_char(  current_date,              '999') ),
       ( to_char(  125::int,                  '999') ),
       ( to_char(  127::int4,                 '999') ),
       ( to_char(  126::int8,                 '999') ),
       ( to_char(  128.8::real,               '999D9') ),
       ( to_char(  125.7::float4,             '999D9') ),
       ( to_char(  125.9::float8,             '999D9') ),
       ( to_char( -125.8::numeric,            '999D99S') );

SELECT COUNT(*) = 19::bigint AS t FROM try;

SELECT like_escape( name, '' ) = like_escape( name::text, '' ) AS t FROM srt;
SELECT like_escape( name::text, ''::citext ) = like_escape( name::text, '' ) AS t FROM srt;
