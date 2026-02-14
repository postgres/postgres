/* skip test if not UTF8 server encoding */
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_bytea_to_text(bytea) RETURNS text
    AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_text_to_bytea(text) RETURNS bytea
    AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_mblen_func(text, text, text, int) RETURNS int
    AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_text_to_wchars(text, text) RETURNS int[]
    AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_wchars_to_text(text, int[]) RETURNS text
    AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_valid_server_encoding(text) RETURNS boolean
    AS :'regresslib' LANGUAGE C STRICT;


CREATE TABLE regress_encoding(good text, truncated text, with_nul text, truncated_with_nul text);
INSERT INTO regress_encoding
VALUES ('café',
        'caf' || test_bytea_to_text('\xc3'),
        'café' || test_bytea_to_text('\x00') || 'dcba',
        'caf' || test_bytea_to_text('\xc300') || 'dcba');

SELECT good, truncated, with_nul FROM regress_encoding;

SELECT length(good) FROM regress_encoding;
SELECT substring(good, 3, 1) FROM regress_encoding;
SELECT substring(good, 4, 1) FROM regress_encoding;
SELECT regexp_replace(good, '^caf(.)$', '\1') FROM regress_encoding;
SELECT reverse(good) FROM regress_encoding;

-- invalid short mb character = error
SELECT length(truncated) FROM regress_encoding;
SELECT substring(truncated, 1, 3) FROM regress_encoding;
SELECT substring(truncated, 1, 4) FROM regress_encoding;
SELECT reverse(truncated) FROM regress_encoding;
-- invalid short mb character = silently dropped
SELECT regexp_replace(truncated, '^caf(.)$', '\1') FROM regress_encoding;

-- PostgreSQL doesn't allow strings to contain NUL.  If a corrupted string
-- contains NUL at a character boundary position, some functions treat it as a
-- character while others treat it as a terminator, as implementation details.

-- NUL = terminator
SELECT length(with_nul) FROM regress_encoding;
SELECT substring(with_nul, 3, 1) FROM regress_encoding;
SELECT substring(with_nul, 4, 1) FROM regress_encoding;
SELECT substring(with_nul, 5, 1) FROM regress_encoding;
SELECT convert_to(substring(with_nul, 5, 1), 'UTF8') FROM regress_encoding;
SELECT regexp_replace(with_nul, '^caf(.)$', '\1') FROM regress_encoding;
-- NUL = character
SELECT with_nul, reverse(with_nul), reverse(reverse(with_nul)) FROM regress_encoding;

-- If a corrupted string contains NUL in the tail bytes of a multibyte
-- character (invalid in all encodings), it is considered part of the
-- character for length purposes.  An error will only be raised in code paths
-- that convert or verify encodings.

SELECT length(truncated_with_nul) FROM regress_encoding;
SELECT substring(truncated_with_nul, 3, 1) FROM regress_encoding;
SELECT substring(truncated_with_nul, 4, 1) FROM regress_encoding;
SELECT convert_to(substring(truncated_with_nul, 4, 1), 'UTF8') FROM regress_encoding;
SELECT substring(truncated_with_nul, 5, 1) FROM regress_encoding;
SELECT regexp_replace(truncated_with_nul, '^caf(.)dcba$', '\1') = test_bytea_to_text('\xc300') FROM regress_encoding;
SELECT reverse(truncated_with_nul) FROM regress_encoding;

-- unbounded: sequence would overrun the string!
SELECT test_mblen_func('pg_mblen_unbounded', 'UTF8', truncated, 3)
FROM regress_encoding;

-- condition detected when using the length/range variants
SELECT test_mblen_func('pg_mblen_with_len', 'UTF8', truncated, 3)
FROM regress_encoding;
SELECT test_mblen_func('pg_mblen_range', 'UTF8', truncated, 3)
FROM regress_encoding;

-- unbounded: sequence would overrun the string, if the terminator were really
-- the end of it
SELECT test_mblen_func('pg_mblen_unbounded', 'UTF8', truncated_with_nul, 3)
FROM regress_encoding;
SELECT test_mblen_func('pg_encoding_mblen', 'GB18030', truncated_with_nul, 3)
FROM regress_encoding;

-- condition detected when using the cstr variants
SELECT test_mblen_func('pg_mblen_cstr', 'UTF8', truncated_with_nul, 3)
FROM regress_encoding;

DROP TABLE regress_encoding;

-- mb<->wchar conversions
CREATE FUNCTION test_encoding(encoding text, description text, input bytea)
RETURNS VOID LANGUAGE plpgsql AS
$$
DECLARE
	prefix text;
	len int;
	wchars int[];
	round_trip bytea;
	result text;
BEGIN
	prefix := rpad(encoding || ' ' || description || ':', 28);

	-- XXX could also test validation, length functions and include client
	-- only encodings with these test cases

	IF test_valid_server_encoding(encoding) THEN
		wchars := test_text_to_wchars(encoding, test_bytea_to_text(input));
		round_trip = test_text_to_bytea(test_wchars_to_text(encoding, wchars));
		if input = round_trip then
			result := 'OK';
		elsif length(input) > length(round_trip) and round_trip = substr(input, 1, length(round_trip)) then
			result := 'truncated';
		else
			result := 'failed';
		end if;
		RAISE NOTICE '% % -> % -> % = %', prefix, input, wchars, round_trip, result;
	END IF;
END;
$$;
-- No validation is done on the encoding itself, just the length to avoid
-- overruns, so some of the byte sequences below are bogus.  They cover
-- all code branches, server encodings only for now.
CREATE TABLE encoding_tests (encoding text, description text, input bytea);
INSERT INTO encoding_tests VALUES
	-- LATIN1, other single-byte encodings
	('LATIN1', 'ASCII',    'a'),
	('LATIN1', 'extended', '\xe9'),
	-- EUC_JP, EUC_JIS_2004, EUR_KR (for the purposes of wchar conversion):
	-- 2 8e (CS2, not used by EUR_KR but arbitrarily considered to have EUC_JP length)
	-- 3 8f (CS3, not used by EUR_KR but arbitrarily considered to have EUC_JP length)
	-- 2 80..ff (CS1)
	('EUC_JP', 'ASCII',      'a'),
	('EUC_JP', 'CS1, short', '\x80'),
	('EUC_JP', 'CS1',        '\x8002'),
	('EUC_JP', 'CS2, short', '\x8e'),
	('EUC_JP', 'CS2',        '\x8e02'),
	('EUC_JP', 'CS3, short', '\x8f'),
	('EUC_JP', 'CS3, short', '\x8f02'),
	('EUC_JP', 'CS3',        '\x8f0203'),
	-- EUC_CN
	-- 3 8e (CS2, not used but arbitrarily considered to have length 3)
	-- 3 8f (CS3, not used but arbitrarily considered to have length 3)
	-- 2 80..ff (CS1)
	('EUC_CN', 'ASCII',      'a'),
	('EUC_CN', 'CS1, short', '\x80'),
	('EUC_CN', 'CS1',        '\x8002'),
	('EUC_CN', 'CS2, short', '\x8e'),
	('EUC_CN', 'CS2, short', '\x8e02'),
	('EUC_CN', 'CS2',        '\x8e0203'),
	('EUC_CN', 'CS3, short', '\x8f'),
	('EUC_CN', 'CS3, short', '\x8f02'),
	('EUC_CN', 'CS3',        '\x8f0203'),
	-- EUC_TW:
	-- 4 8e (CS2)
	-- 3 8f (CS3, not used but arbitrarily considered to have length 3)
	-- 2 80..ff (CS1)
	('EUC_TW', 'ASCII',      'a'),
	('EUC_TW', 'CS1, short', '\x80'),
	('EUC_TW', 'CS1',        '\x8002'),
	('EUC_TW', 'CS2, short', '\x8e'),
	('EUC_TW', 'CS2, short', '\x8e02'),
	('EUC_TW', 'CS2, short', '\x8e0203'),
	('EUC_TW', 'CS2',        '\x8e020304'),
	('EUC_TW', 'CS3, short', '\x8f'),
	('EUC_TW', 'CS3, short', '\x8f02'),
	('EUC_TW', 'CS3',        '\x8f0203'),
	-- UTF8
	-- 2 c0..df
	-- 3 e0..ef
	-- 4 f0..f7 (but maximum real codepoint U+10ffff has f4)
	-- 5 f8..fb (not supported)
	-- 6 fc..fd (not supported)
	('UTF8',   'ASCII',               'a'),
	('UTF8',   '2 byte, short',       '\xdf'),
	('UTF8',   '2 byte',              '\xdf82'),
	('UTF8',   '3 byte, short',       '\xef'),
	('UTF8',   '3 byte, short',       '\xef82'),
	('UTF8',   '3 byte',              '\xef8283'),
	('UTF8',   '4 byte, short',       '\xf7'),
	('UTF8',   '4 byte, short',       '\xf782'),
	('UTF8',   '4 byte, short',       '\xf78283'),
	('UTF8',   '4 byte',              '\xf7828384'),
	('UTF8',   '5 byte, unsupported', '\xfb'),
	('UTF8',   '5 byte, unsupported', '\xfb82'),
	('UTF8',   '5 byte, unsupported', '\xfb8283'),
	('UTF8',   '5 byte, unsupported', '\xfb828384'),
	('UTF8',   '5 byte, unsupported', '\xfb82838485'),
	('UTF8',   '6 byte, unsupported', '\xfd'),
	('UTF8',   '6 byte, unsupported', '\xfd82'),
	('UTF8',   '6 byte, unsupported', '\xfd8283'),
	('UTF8',   '6 byte, unsupported', '\xfd828384'),
	('UTF8',   '6 byte, unsupported', '\xfd82838485'),
	('UTF8',   '6 byte, unsupported', '\xfd8283848586'),
	-- MULE_INTERNAL
	-- 2 81..8d LC1
	-- 3 90..99 LC2
	('MULE_INTERNAL', 'ASCII',         'a'),
	('MULE_INTERNAL', 'LC1, short',    '\x81'),
	('MULE_INTERNAL', 'LC1',           '\x8182'),
	('MULE_INTERNAL', 'LC2, short',    '\x90'),
	('MULE_INTERNAL', 'LC2, short',    '\x9082'),
	('MULE_INTERNAL', 'LC2',           '\x908283');

SELECT COUNT(test_encoding(encoding, description, input)) > 0
FROM encoding_tests;

-- substring fetches a slice of a toasted value; unused tail of that slice is
-- an incomplete char (bug #19406)
CREATE TABLE toast_3b_utf8 (c text);
INSERT INTO toast_3b_utf8 VALUES (repeat(U&'\2026', 4000));
SELECT SUBSTRING(c FROM 1 FOR 1) FROM toast_3b_utf8;
SELECT SUBSTRING(c FROM 4001 FOR 1) FROM toast_3b_utf8;
-- diagnose incomplete char iff within the substring
UPDATE toast_3b_utf8 SET c = c || test_bytea_to_text('\xe280');
SELECT SUBSTRING(c FROM 4000 FOR 1) FROM toast_3b_utf8;
SELECT SUBSTRING(c FROM 4001 FOR 1) FROM toast_3b_utf8;
-- substring needing last byte of its slice_size
ALTER TABLE toast_3b_utf8 RENAME TO toast_4b_utf8;
UPDATE toast_4b_utf8 SET c = repeat(U&'\+01F680', 3000);
SELECT SUBSTRING(c FROM 3000 FOR 1) FROM toast_4b_utf8;

DROP TABLE encoding_tests;
DROP TABLE toast_4b_utf8;
DROP FUNCTION test_encoding;
DROP FUNCTION test_text_to_wchars;
DROP FUNCTION test_mblen_func;
DROP FUNCTION test_bytea_to_text;
DROP FUNCTION test_text_to_bytea;


-- substring slow path: multi-byte escape char vs. multi-byte pattern char.
SELECT SUBSTRING('a' SIMILAR U&'\00AC' ESCAPE U&'\00A7');
-- Levenshtein distance metric: exercise character length cache.
SELECT U&"real\00A7_name" FROM (select 1) AS x(real_name);
-- JSON errcontext: truncate long data.
SELECT repeat(U&'\00A7', 30)::json;
