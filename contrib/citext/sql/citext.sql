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
SELECT citext_cmp('B'::citext, 'a'::citext) AS one;

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

-- LIKE shoudl be case-insensitive
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
SELECT regexp_replace('Thomas'::citext, '.[mN]a.', 'M') = 'ThM' AS t;
SELECT regexp_split_to_array('hello world'::citext, E'\\s+') = ARRAY[ 'hello', 'world' ] AS t;
SELECT regexp_split_to_table('hello world'::citext, E'\\s+') AS words;

SELECT repeat('Pg'::citext, 4) = 'PgPgPgPg' AS t;
SELECT replace('abcdefabcdef'::citext, 'cd', 'XX') = 'abXXefabXXef' AS t;

SELECT rpad('hi'::citext, 5              ) = 'hi   ' AS t;
SELECT rpad('hi'::citext, 5, 'xy'::citext) = 'hixyx' AS t;
SELECT rpad('hi'::text,   5, 'xy'::citext) = 'hixyx' AS t;
SELECT rpad('hi'::citext, 5, 'xy'::text  ) = 'hixyx' AS t;

SELECT rtrim('trim    '::citext             ) = 'trim' AS t;
SELECT rtrim('trimxxxx'::citext, 'x'::citext) = 'trim' AS t;
SELECT rtrim('trimxxxx'::text,   'x'::citext) = 'trim' AS t;
SELECT rtrim('trimxxxx'::text,   'x'::text  ) = 'trim' AS t;

SELECT split_part('abc~@~def~@~ghi'::citext, '~@~', 2) = 'def' AS t;
SELECT strpos('high'::citext, 'ig'        ) = 2 AS t;
SELECT strpos('high'::citext, 'ig'::citext) = 2 AS t;
-- to_ascii() does not support UTF-8.
-- to_hex() takes a numeric argument.
SELECT substr('alphabet', 3, 2) = 'ph' AS t;
SELECT translate('abcdefabcdef'::citext, 'cd', 'XX') = 'abXXefabXXef' AS t;

-- TODO These functions should work case-insensitively, but don't.
SELECT regexp_matches('foobarbequebaz'::citext, '(BAR)(BEQUE)') = ARRAY[ 'bar', 'beque' ] AS "t TODO";
SELECT regexp_replace('Thomas'::citext, '.[MN]A.', 'M') = 'THM' AS "t TODO";
SELECT regexp_split_to_array('helloTworld'::citext, 't') = ARRAY[ 'hello', 'world' ] AS "t TODO";
SELECT regexp_split_to_table('helloTworld'::citext, 't') AS "words TODO";
SELECT replace('abcdefabcdef'::citext, 'CD', 'XX') = 'abXXefabXXef' AS "t TODO";
SELECT split_part('abcTdefTghi'::citext, 't', 2) = 'def' AS "t TODO";
SELECT strpos('high'::citext, 'IG'::citext) = 2 AS "t TODO";
SELECT translate('abcdefabcdef'::citext, 'CD', 'XX') = 'abXXefabXXef' AS "t TODO";

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
SELECT like_escape( name::text, ''::citext ) =like_escape( name::text, '' ) AS t FROM srt;

--- TODO: Get citext working with magic cast functions?
SELECT cidr( '192.168.1.2'::citext ) = cidr( '192.168.1.2'::text ) AS "t TODO";
SELECT '192.168.1.2'::cidr::citext = '192.168.1.2'::cidr::text AS "t TODO";
