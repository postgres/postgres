-- Default set of tests for TOAST compression, independent on compression
-- methods supported by the build.

CREATE SCHEMA pglz;
SET search_path TO pglz, public;
\set HIDE_TOAST_COMPRESSION false

-- ensure we get stable results regardless of installation's default
SET default_toast_compression = 'pglz';

-- test creating table with compression method
CREATE TABLE cmdata(f1 text COMPRESSION pglz);
CREATE INDEX idx ON cmdata(f1);
INSERT INTO cmdata VALUES(repeat('1234567890', 1000));
\d+ cmdata

-- verify stored compression method in the data
SELECT pg_column_compression(f1) FROM cmdata;

-- decompress data slice
SELECT SUBSTR(f1, 200, 5) FROM cmdata;

-- copy with table creation
SELECT * INTO cmmove1 FROM cmdata;
\d+ cmmove1
SELECT pg_column_compression(f1) FROM cmmove1;

-- try setting compression for incompressible data type
CREATE TABLE cmdata2 (f1 int COMPRESSION pglz);

-- test externally stored compressed data
CREATE OR REPLACE FUNCTION large_val() RETURNS TEXT LANGUAGE SQL AS
'select array_agg(fipshash(g::text))::text from generate_series(1, 256) g';
CREATE TABLE cmdata2 (f1 text COMPRESSION pglz);
INSERT INTO cmdata2 SELECT large_val() || repeat('a', 4000);
SELECT pg_column_compression(f1) FROM cmdata2;
SELECT SUBSTR(f1, 200, 5) FROM cmdata2;
DROP TABLE cmdata2;

--test column type update varlena/non-varlena
CREATE TABLE cmdata2 (f1 int);
\d+ cmdata2
ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE varchar;
\d+ cmdata2
ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE int USING f1::integer;
\d+ cmdata2

--changing column storage should not impact the compression method
--but the data should not be compressed
ALTER TABLE cmdata2 ALTER COLUMN f1 TYPE varchar;
ALTER TABLE cmdata2 ALTER COLUMN f1 SET COMPRESSION pglz;
\d+ cmdata2
ALTER TABLE cmdata2 ALTER COLUMN f1 SET STORAGE plain;
\d+ cmdata2
INSERT INTO cmdata2 VALUES (repeat('123456789', 800));
SELECT pg_column_compression(f1) FROM cmdata2;

-- test compression with inheritance
CREATE TABLE cmdata3(f1 text);
CREATE TABLE cminh() INHERITS (cmdata, cmdata3);

-- test default_toast_compression GUC
-- suppress machine-dependent details
\set VERBOSITY terse
SET default_toast_compression = '';
SET default_toast_compression = 'I do not exist compression';
SET default_toast_compression = 'pglz';
\set VERBOSITY default

ALTER TABLE cmdata2 ALTER COLUMN f1 SET COMPRESSION default;
\d+ cmdata2

DROP TABLE cmdata2;

-- VACUUM FULL does not recompress
SELECT pg_column_compression(f1) FROM cmdata;
VACUUM FULL cmdata;
SELECT pg_column_compression(f1) FROM cmdata;

-- check data is ok
SELECT length(f1) FROM cmdata;
SELECT length(f1) FROM cmmove1;

CREATE TABLE badcompresstbl (a text COMPRESSION I_Do_Not_Exist_Compression); -- fails
CREATE TABLE badcompresstbl (a text);
ALTER TABLE badcompresstbl ALTER a SET COMPRESSION I_Do_Not_Exist_Compression; -- fails
DROP TABLE badcompresstbl;

\set HIDE_TOAST_COMPRESSION true
