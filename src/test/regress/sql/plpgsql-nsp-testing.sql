-- nspname.relname.attname%TYPE
DROP FUNCTION t();
CREATE OR REPLACE FUNCTION t() RETURNS TEXT AS '
DECLARE
    col_name pg_catalog.pg_attribute.attname%TYPE;
BEGIN
    col_name := ''uga'';
    RETURN col_name;
END;
' LANGUAGE 'plpgsql';
SELECT t();

-- nspname.relname%ROWTYPE
DROP FUNCTION t();
CREATE OR REPLACE FUNCTION t() RETURNS pg_catalog.pg_attribute AS '
DECLARE
    rec pg_catalog.pg_attribute%ROWTYPE;
BEGIN
    SELECT INTO rec * FROM pg_catalog.pg_attribute WHERE attrelid = 1247 AND attname = ''typname'';
    RETURN rec;
END;
' LANGUAGE 'plpgsql';
SELECT * FROM t();

-- nspname.relname.attname%TYPE
DROP FUNCTION t();
CREATE OR REPLACE FUNCTION t() RETURNS pg_catalog.pg_attribute.attname%TYPE AS '
DECLARE
    rec pg_catalog.pg_attribute.attname%TYPE;
BEGIN
    SELECT INTO rec pg_catalog.pg_attribute.attname FROM pg_catalog.pg_attribute WHERE attrelid = 1247 AND attname = ''typname'';
    RETURN rec;
END;
' LANGUAGE 'plpgsql';
SELECT t();

-- nspname.relname%ROWTYPE
DROP FUNCTION t();
CREATE OR REPLACE FUNCTION t() RETURNS pg_catalog.pg_attribute AS '
DECLARE
    rec pg_catalog.pg_attribute%ROWTYPE;
BEGIN
    SELECT INTO rec * FROM pg_catalog.pg_attribute WHERE attrelid = 1247 AND attname = ''typname'';
    RETURN rec;
END;
' LANGUAGE 'plpgsql';
SELECT * FROM t();
