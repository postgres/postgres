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

DROP TABLE copy_encoding_tab;
