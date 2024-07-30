/*
 * This test is for collations and character operations when using the
 * builtin provider with the C.UTF-8 locale.
 */

/* skip test if not UTF8 server encoding */
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

SET client_encoding TO UTF8;

--
-- Test builtin "C"
--
CREATE COLLATION regress_builtin_c (
  provider = builtin, locale = 'C');

-- non-ASCII characters are unchanged
SELECT LOWER(U&'\00C1' COLLATE regress_builtin_c) = U&'\00C1';
SELECT UPPER(U&'\00E1' COLLATE regress_builtin_c) = U&'\00E1';

-- non-ASCII characters are not alphabetic
SELECT U&'\00C1\00E1' !~ '[[:alpha:]]' COLLATE regress_builtin_c;

DROP COLLATION regress_builtin_c;

--
-- Test PG_C_UTF8
--

CREATE COLLATION regress_pg_c_utf8 (
  provider = builtin, locale = 'C_UTF8'); -- fails
CREATE COLLATION regress_pg_c_utf8 (
  provider = builtin, locale = 'C.UTF8');
DROP COLLATION regress_pg_c_utf8;
CREATE COLLATION regress_pg_c_utf8 (
  provider = builtin, locale = 'C.UTF-8');

CREATE TABLE test_pg_c_utf8 (
  t TEXT COLLATE PG_C_UTF8
);
INSERT INTO test_pg_c_utf8 VALUES
  ('abc DEF 123abc'),
  ('ábc sßs ßss DÉF'),
  ('ǄxxǄ ǆxxǅ ǅxxǆ'),
  ('ȺȺȺ'),
  ('ⱥⱥⱥ'),
  ('ⱥȺ');

SELECT
    t, lower(t), initcap(t), upper(t),
    length(convert_to(t, 'UTF8')) AS t_bytes,
    length(convert_to(lower(t), 'UTF8')) AS lower_t_bytes,
    length(convert_to(initcap(t), 'UTF8')) AS initcap_t_bytes,
    length(convert_to(upper(t), 'UTF8')) AS upper_t_bytes
  FROM test_pg_c_utf8;

DROP TABLE test_pg_c_utf8;

-- negative test: Final_Sigma not used for builtin locale C.UTF-8
SELECT lower('ΑΣ' COLLATE PG_C_UTF8);
SELECT lower('ΑͺΣͺ' COLLATE PG_C_UTF8);
SELECT lower('Α΄Σ΄' COLLATE PG_C_UTF8);

-- properties

SELECT 'xyz' ~ '[[:alnum:]]' COLLATE PG_C_UTF8;
SELECT 'xyz' !~ '[[:upper:]]' COLLATE PG_C_UTF8;
SELECT '@' !~ '[[:alnum:]]' COLLATE PG_C_UTF8;
SELECT '=' ~ '[[:punct:]]' COLLATE PG_C_UTF8; -- symbols are punctuation in posix
SELECT 'a8a' ~ '[[:digit:]]' COLLATE PG_C_UTF8;
SELECT '൧' !~ '\d' COLLATE PG_C_UTF8; -- only 0-9 considered digits in posix

-- case mapping

SELECT 'xYz' ~* 'XyZ' COLLATE PG_C_UTF8;
SELECT 'xAb' ~* '[W-Y]' COLLATE PG_C_UTF8;
SELECT 'xAb' !~* '[c-d]' COLLATE PG_C_UTF8;
SELECT 'Δ' ~* '[γ-λ]' COLLATE PG_C_UTF8;
SELECT 'δ' ~* '[Γ-Λ]' COLLATE PG_C_UTF8; -- same as above with cases reversed
