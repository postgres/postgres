--
-- Test cases for encoding with COPY commands
--

-- skip test if not UTF8 server encoding
SELECT getdatabaseencoding() <> 'UTF8'
       AS skip_test \gset
\if :skip_test
\quit
\endif

-- directory paths are passed to us in environment variables
\getenv abs_builddir PG_ABS_BUILDDIR

\set utf8_csv :abs_builddir '/results/copyencoding_utf8.csv'

CREATE TABLE copy_encoding_tab (t text);

-- Valid cases

-- Use ENCODING option
-- U+3042 HIRAGANA LETTER A
COPY (SELECT E'\u3042') TO :'utf8_csv' WITH (FORMAT csv, ENCODING 'UTF8');
-- Read UTF8 data as LATIN1: no error
COPY copy_encoding_tab FROM :'utf8_csv' WITH (FORMAT csv, ENCODING 'LATIN1');
-- Non-server encodings have distinct code paths.
\set fname :abs_builddir '/results/copyencoding_gb18030.csv'
COPY (SELECT E'\u3042,') TO :'fname' WITH (FORMAT csv, ENCODING 'GB18030');
COPY copy_encoding_tab FROM :'fname' WITH (FORMAT csv, ENCODING 'GB18030');
\set fname :abs_builddir '/results/copyencoding_gb18030.data'
COPY (SELECT E'\u3042,') TO :'fname' WITH (FORMAT text, ENCODING 'GB18030');
COPY copy_encoding_tab FROM :'fname' WITH (FORMAT text, ENCODING 'GB18030');

-- Use client_encoding
SET client_encoding TO UTF8;
-- U+3042 HIRAGANA LETTER A
COPY (SELECT E'\u3042') TO :'utf8_csv' WITH (FORMAT csv);
-- Read UTF8 data as LATIN1: no error
SET client_encoding TO LATIN1;
COPY copy_encoding_tab FROM :'utf8_csv' WITH (FORMAT csv);
RESET client_encoding;

-- Invalid cases

-- Use ENCODING explicitly
-- U+3042 HIRAGANA LETTER A
COPY (SELECT E'\u3042') TO :'utf8_csv' WITH (FORMAT csv, ENCODING 'UTF8');
-- Read UTF8 data as EUC_JP: no error
COPY copy_encoding_tab FROM :'utf8_csv' WITH (FORMAT csv, ENCODING 'EUC_JP');

-- Use client_encoding
SET client_encoding TO UTF8;
-- U+3042 HIRAGANA LETTER A
COPY (SELECT E'\u3042') TO :'utf8_csv' WITH (FORMAT csv);
-- Read UTF8 data as EUC_JP: no error
SET client_encoding TO EUC_JP;
COPY copy_encoding_tab FROM :'utf8_csv' WITH (FORMAT csv);
RESET client_encoding;

-- JSON format encoding conversion
\set json_latin1 :abs_builddir '/results/copyencoding_json_latin1.json'
COPY (SELECT E'\u00e9' AS c) TO :'json_latin1' WITH (FORMAT json, ENCODING 'LATIN1');
-- Verify the file contains LATIN1 'é' (single byte 0xe9) and not UTF-8 (0xc3 0xa9).
-- Done as separate position checks to stay independent of the platform's
-- end-of-line convention.
SELECT position('\xe9'::bytea  IN pg_read_binary_file(:'json_latin1')) > 0 AS has_latin1_e9,
       position('\xc3a9'::bytea IN pg_read_binary_file(:'json_latin1')) > 0 AS has_utf8_e9;

-- Same with implicit encoding inherited from client_encoding (no ENCODING
-- option).  Covers the case where a client with a non-UTF8 client_encoding
-- runs COPY ... FORMAT json and would otherwise receive unconverted bytes.
\set json_implicit :abs_builddir '/results/copyencoding_json_implicit_latin1.json'
SET client_encoding TO LATIN1;
COPY (SELECT E'\u00e9' AS c) TO :'json_implicit' WITH (FORMAT json);
RESET client_encoding;
SELECT position('\xe9'::bytea  IN pg_read_binary_file(:'json_implicit')) > 0 AS has_latin1_e9,
       position('\xc3a9'::bytea IN pg_read_binary_file(:'json_implicit')) > 0 AS has_utf8_e9;

DROP TABLE copy_encoding_tab;
