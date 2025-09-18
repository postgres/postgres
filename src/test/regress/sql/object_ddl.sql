--
-- Test for the following functions to get object DDL:
-- - pg_get_domain_ddl
--

CREATE DOMAIN regress_us_postal_code AS TEXT
    DEFAULT '00000'
    CONSTRAINT regress_us_postal_code_check
        CHECK (
            VALUE ~ '^\d{5}$'
    OR VALUE ~ '^\d{5}-\d{4}$'
    );

SELECT pg_get_domain_ddl('regress_us_postal_code');


CREATE DOMAIN regress_domain_not_null AS INT NOT NULL;

SELECT pg_get_domain_ddl('regress_domain_not_null');


CREATE DOMAIN regress_domain_check AS INT
    CONSTRAINT regress_a CHECK (VALUE < 100)
    CONSTRAINT regress_b CHECK (VALUE > 10);

SELECT pg_get_domain_ddl('regress_domain_check');


CREATE DOMAIN "regress_domain with space" AS INT
    CONSTRAINT regress_a CHECK (VALUE < 100)
    CONSTRAINT "regress_Constraint B" CHECK (VALUE > 10)
    CONSTRAINT "regress_ConstraintC" CHECK (VALUE != 55);

SELECT pg_get_domain_ddl('"regress_domain with space"');

-- Test error cases
SELECT pg_get_domain_ddl('regress_nonexistent_domain'::regtype);  -- should fail
SELECT pg_get_domain_ddl(NULL);  -- should return NULL

-- Test domains with no constraints
CREATE DOMAIN regress_simple_domain AS text;
SELECT pg_get_domain_ddl('regress_simple_domain');

-- Test domain over another domain
CREATE DOMAIN regress_base_domain AS varchar(10);
CREATE DOMAIN regress_derived_domain AS regress_base_domain CHECK (LENGTH(VALUE) > 3);
SELECT pg_get_domain_ddl('regress_derived_domain');

-- Test domain with complex default expressions
CREATE SEQUENCE regress_test_seq;
CREATE DOMAIN regress_seq_domain AS int DEFAULT nextval('regress_test_seq');
SELECT pg_get_domain_ddl('regress_seq_domain');

-- Test domain with a renamed sequence as default expression
ALTER SEQUENCE regress_test_seq RENAME TO regress_test_seq_renamed;
SELECT pg_get_domain_ddl('regress_seq_domain');

-- Test domain with type modifiers
CREATE DOMAIN regress_precise_numeric AS numeric(10,2) DEFAULT 0.00;
SELECT pg_get_domain_ddl('regress_precise_numeric');

-- Test domain over array type
CREATE DOMAIN regress_int_array_domain AS int[] CHECK (array_length(VALUE, 1) <= 5);
SELECT pg_get_domain_ddl('regress_int_array_domain');

-- Test domain in non-public schema
CREATE SCHEMA regress_test_schema;
CREATE DOMAIN regress_test_schema.regress_schema_domain AS text DEFAULT 'test';
SELECT pg_get_domain_ddl('regress_test_schema.regress_schema_domain');

-- Test domain with multiple constraint types combined
CREATE DOMAIN regress_comprehensive_domain AS varchar(50)
    NOT NULL
    DEFAULT 'default_value'
    CHECK (LENGTH(VALUE) >= 5)
    CHECK (VALUE !~ '^\s*$');  -- not just whitespace
SELECT pg_get_domain_ddl('regress_comprehensive_domain');

-- Test domain over composite type
CREATE TYPE regress_address_type AS (street text, city text, zipcode text);
CREATE DOMAIN regress_address_domain AS regress_address_type CHECK ((VALUE).zipcode ~ '^\d{5}$');
SELECT pg_get_domain_ddl('regress_address_domain');

-- Cleanup
DROP DOMAIN regress_us_postal_code;
DROP DOMAIN regress_domain_not_null;
DROP DOMAIN regress_domain_check;
DROP DOMAIN "regress_domain with space";
DROP DOMAIN regress_comprehensive_domain;
DROP DOMAIN regress_test_schema.regress_schema_domain;
DROP SCHEMA regress_test_schema;
DROP DOMAIN regress_address_domain;
DROP TYPE regress_address_type;
DROP DOMAIN regress_int_array_domain;
DROP DOMAIN regress_precise_numeric;
DROP DOMAIN regress_seq_domain;
DROP SEQUENCE regress_test_seq_renamed;
DROP DOMAIN regress_derived_domain;
DROP DOMAIN regress_base_domain;
DROP DOMAIN regress_simple_domain;
