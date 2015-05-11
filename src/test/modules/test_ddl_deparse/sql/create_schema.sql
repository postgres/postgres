--
-- CREATE_SCHEMA
--

CREATE SCHEMA foo;

CREATE SCHEMA IF NOT EXISTS bar;

CREATE SCHEMA baz;

-- Will not be created, and will not be handled by the
-- event trigger
CREATE SCHEMA IF NOT EXISTS baz;

CREATE SCHEMA element_test
  CREATE TABLE foo (id int)
  CREATE VIEW bar AS SELECT * FROM foo;
