--
-- Enum tests
--

CREATE TYPE rainbow AS ENUM ('red', 'orange', 'yellow', 'green', 'blue', 'purple');

--
-- Did it create the right number of rows?
--
SELECT COUNT(*) FROM pg_enum WHERE enumtypid = 'rainbow'::regtype;

--
-- I/O functions
--
SELECT 'red'::rainbow;
SELECT 'mauve'::rainbow;

--
-- Basic table creation, row selection
--
CREATE TABLE enumtest (col rainbow);
INSERT INTO enumtest values ('red'), ('orange'), ('yellow'), ('green');
COPY enumtest FROM stdin;
blue
purple
\.
SELECT * FROM enumtest;

--
-- Operators, no index
--
SELECT * FROM enumtest WHERE col = 'orange';
SELECT * FROM enumtest WHERE col <> 'orange' ORDER BY col;
SELECT * FROM enumtest WHERE col > 'yellow' ORDER BY col;
SELECT * FROM enumtest WHERE col >= 'yellow' ORDER BY col;
SELECT * FROM enumtest WHERE col < 'green' ORDER BY col;
SELECT * FROM enumtest WHERE col <= 'green' ORDER BY col;

--
-- Cast to/from text
--
SELECT 'red'::rainbow::text || 'hithere';
SELECT 'red'::text::rainbow = 'red'::rainbow;

--
-- Aggregates
--
SELECT min(col) FROM enumtest;
SELECT max(col) FROM enumtest;
SELECT max(col) FROM enumtest WHERE col < 'green';

--
-- Index tests, force use of index
--
SET enable_seqscan = off;
SET enable_bitmapscan = off;

--
-- Btree index / opclass with the various operators
--
CREATE UNIQUE INDEX enumtest_btree ON enumtest USING btree (col);
SELECT * FROM enumtest WHERE col = 'orange';
SELECT * FROM enumtest WHERE col <> 'orange' ORDER BY col;
SELECT * FROM enumtest WHERE col > 'yellow' ORDER BY col;
SELECT * FROM enumtest WHERE col >= 'yellow' ORDER BY col;
SELECT * FROM enumtest WHERE col < 'green' ORDER BY col;
SELECT * FROM enumtest WHERE col <= 'green' ORDER BY col;
SELECT min(col) FROM enumtest;
SELECT max(col) FROM enumtest;
SELECT max(col) FROM enumtest WHERE col < 'green';
DROP INDEX enumtest_btree;

--
-- Hash index / opclass with the = operator
--
CREATE INDEX enumtest_hash ON enumtest USING hash (col);
SELECT * FROM enumtest WHERE col = 'orange';
DROP INDEX enumtest_hash;

--
-- End index tests
--
RESET enable_seqscan;
RESET enable_bitmapscan;

--
-- Domains over enums
--
CREATE DOMAIN rgb AS rainbow CHECK (VALUE IN ('red', 'green', 'blue'));
SELECT 'red'::rgb;
SELECT 'purple'::rgb;
SELECT 'purple'::rainbow::rgb;
DROP DOMAIN rgb;

--
-- Arrays
--
SELECT '{red,green,blue}'::rainbow[];
SELECT ('{red,green,blue}'::rainbow[])[2];
SELECT 'red' = ANY ('{red,green,blue}'::rainbow[]);
SELECT 'yellow' = ANY ('{red,green,blue}'::rainbow[]);
SELECT 'red' = ALL ('{red,green,blue}'::rainbow[]);
SELECT 'red' = ALL ('{red,red}'::rainbow[]);

--
-- Support functions
--
SELECT enum_first(NULL::rainbow);
SELECT enum_last('green'::rainbow);
SELECT enum_range(NULL::rainbow);
SELECT enum_range('orange'::rainbow, 'green'::rainbow);
SELECT enum_range(NULL, 'green'::rainbow);
SELECT enum_range('orange'::rainbow, NULL);
SELECT enum_range(NULL::rainbow, NULL);

--
-- User functions, can't test perl/python etc here since may not be compiled.
--
CREATE FUNCTION echo_me(anyenum) RETURNS text AS $$
BEGIN
RETURN $1::text || 'omg';
END
$$ LANGUAGE plpgsql;
SELECT echo_me('red'::rainbow);
--
-- Concrete function should override generic one
--
CREATE FUNCTION echo_me(rainbow) RETURNS text AS $$
BEGIN
RETURN $1::text || 'wtf';
END
$$ LANGUAGE plpgsql;
SELECT echo_me('red'::rainbow);
--
-- If we drop the original generic one, we don't have to qualify the type
-- anymore, since there's only one match
--
DROP FUNCTION echo_me(anyenum);
SELECT echo_me('red');
DROP FUNCTION echo_me(rainbow);

--
-- RI triggers on enum types
--
CREATE TABLE enumtest_parent (id rainbow PRIMARY KEY);
CREATE TABLE enumtest_child (parent rainbow REFERENCES enumtest_parent);
INSERT INTO enumtest_parent VALUES ('red');
INSERT INTO enumtest_child VALUES ('red');
INSERT INTO enumtest_child VALUES ('blue');  -- fail
DELETE FROM enumtest_parent;  -- fail
--
-- cross-type RI should fail
--
CREATE TYPE bogus AS ENUM('good', 'bad', 'ugly');
CREATE TABLE enumtest_bogus_child(parent bogus REFERENCES enumtest_parent);
DROP TYPE bogus;

--
-- Cleanup
--
DROP TABLE enumtest_child;
DROP TABLE enumtest_parent;
DROP TABLE enumtest;
DROP TYPE rainbow;

--
-- Verify properly cleaned up
--
SELECT COUNT(*) FROM pg_type WHERE typname = 'rainbow';
SELECT * FROM pg_enum WHERE NOT EXISTS
  (SELECT 1 FROM pg_type WHERE pg_type.oid = enumtypid);
