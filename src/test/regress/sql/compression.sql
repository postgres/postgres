\set HIDE_TOAST_COMPRESSION false

-- ensure we get stable results regardless of installation's default
SET default_toast_compression = 'pglz';

-- test creating table with compression method
CREATE TABLE cmdata(f1 text COMPRESSION pglz);
CREATE INDEX idx ON cmdata(f1);
INSERT INTO cmdata VALUES(repeat('1234567890', 1000));
\d+ cmdata
CREATE TABLE cmdata1(f1 TEXT COMPRESSION lz4);
INSERT INTO cmdata1 VALUES(repeat('1234567890', 1004));
\d+ cmdata1

-- verify stored compression method in the data
SELECT pg_column_compression(f1) FROM cmdata;
SELECT pg_column_compression(f1) FROM cmdata1;

-- decompress data slice
SELECT SUBSTR(f1, 200, 5) FROM cmdata;
SELECT SUBSTR(f1, 2000, 50) FROM cmdata1;

-- copy with table creation
SELECT * INTO cmmove1 FROM cmdata;
\d+ cmmove1
SELECT pg_column_compression(f1) FROM cmmove1;

-- copy to existing table
CREATE TABLE cmmove3(f1 text COMPRESSION pglz);
INSERT INTO cmmove3 SELECT * FROM cmdata;
INSERT INTO cmmove3 SELECT * FROM cmdata1;
SELECT pg_column_compression(f1) FROM cmmove3;

-- test LIKE INCLUDING COMPRESSION
CREATE TABLE cmdata2 (LIKE cmdata1 INCLUDING COMPRESSION);
\d+ cmdata2
DROP TABLE cmdata2;

-- try setting compression for incompressible data type
CREATE TABLE cmdata2 (f1 int COMPRESSION pglz);

-- update using datum from different table
CREATE TABLE cmmove2(f1 text COMPRESSION pglz);
INSERT INTO cmmove2 VALUES (repeat('1234567890', 1004));
SELECT pg_column_compression(f1) FROM cmmove2;
UPDATE cmmove2 SET f1 = cmdata1.f1 FROM cmdata1;
SELECT pg_column_compression(f1) FROM cmmove2;

-- test externally stored compressed data
CREATE OR REPLACE FUNCTION large_val() RETURNS TEXT LANGUAGE SQL AS
'select array_agg(fipshash(g::text))::text from generate_series(1, 256) g';
CREATE TABLE cmdata2 (f1 text COMPRESSION pglz);
INSERT INTO cmdata2 SELECT large_val() || repeat('a', 4000);
SELECT pg_column_compression(f1) FROM cmdata2;
INSERT INTO cmdata1 SELECT large_val() || repeat('a', 4000);
SELECT pg_column_compression(f1) FROM cmdata1;
SELECT SUBSTR(f1, 200, 5) FROM cmdata1;
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

-- test compression with materialized view
CREATE MATERIALIZED VIEW compressmv(x) AS SELECT * FROM cmdata1;
\d+ compressmv
SELECT pg_column_compression(f1) FROM cmdata1;
SELECT pg_column_compression(x) FROM compressmv;

-- test compression with partition
CREATE TABLE cmpart(f1 text COMPRESSION lz4) PARTITION BY HASH(f1);
CREATE TABLE cmpart1 PARTITION OF cmpart FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE cmpart2(f1 text COMPRESSION pglz);

ALTER TABLE cmpart ATTACH PARTITION cmpart2 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- test compression with inheritance
CREATE TABLE cminh() INHERITS(cmdata, cmdata1); -- error
CREATE TABLE cminh(f1 TEXT COMPRESSION lz4) INHERITS(cmdata); -- error
CREATE TABLE cmdata3(f1 text);
CREATE TABLE cminh() INHERITS (cmdata, cmdata3);

-- test default_toast_compression GUC
SET default_toast_compression = '';
SET default_toast_compression = 'I do not exist compression';
SET default_toast_compression = 'lz4';
SET default_toast_compression = 'pglz';

-- test alter compression method
ALTER TABLE cmdata ALTER COLUMN f1 SET COMPRESSION lz4;
INSERT INTO cmdata VALUES (repeat('123456789', 4004));
\d+ cmdata
SELECT pg_column_compression(f1) FROM cmdata;

ALTER TABLE cmdata2 ALTER COLUMN f1 SET COMPRESSION default;
\d+ cmdata2

-- test alter compression method for materialized views
ALTER MATERIALIZED VIEW compressmv ALTER COLUMN x SET COMPRESSION lz4;
\d+ compressmv

-- test alter compression method for partitioned tables
ALTER TABLE cmpart1 ALTER COLUMN f1 SET COMPRESSION pglz;
ALTER TABLE cmpart2 ALTER COLUMN f1 SET COMPRESSION lz4;

-- new data should be compressed with the current compression method
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- VACUUM FULL does not recompress
SELECT pg_column_compression(f1) FROM cmdata;
VACUUM FULL cmdata;
SELECT pg_column_compression(f1) FROM cmdata;

-- test expression index
DROP TABLE cmdata2;
CREATE TABLE cmdata2 (f1 TEXT COMPRESSION pglz, f2 TEXT COMPRESSION lz4);
CREATE UNIQUE INDEX idx1 ON cmdata2 ((f1 || f2));
INSERT INTO cmdata2 VALUES((SELECT array_agg(fipshash(g::TEXT))::TEXT FROM
generate_series(1, 50) g), VERSION());

-- check data is ok
SELECT length(f1) FROM cmdata;
SELECT length(f1) FROM cmdata1;
SELECT length(f1) FROM cmmove1;
SELECT length(f1) FROM cmmove2;
SELECT length(f1) FROM cmmove3;

CREATE TABLE badcompresstbl (a text COMPRESSION I_Do_Not_Exist_Compression); -- fails
CREATE TABLE badcompresstbl (a text);
ALTER TABLE badcompresstbl ALTER a SET COMPRESSION I_Do_Not_Exist_Compression; -- fails
DROP TABLE badcompresstbl;

\set HIDE_TOAST_COMPRESSION true
