--
-- IF EXISTS tests
--

-- table (will be really dropped at the end)

DROP TABLE test_exists;

DROP TABLE IF EXISTS test_exists;

CREATE TABLE test_exists (a int, b text);

-- view

DROP VIEW test_view_exists;

DROP VIEW IF EXISTS test_view_exists;

CREATE VIEW test_view_exists AS select * from test_exists;

DROP VIEW IF EXISTS test_view_exists;

DROP VIEW test_view_exists;

-- index

DROP INDEX test_index_exists;

DROP INDEX IF EXISTS test_index_exists;

CREATE INDEX test_index_exists on test_exists(a);

DROP INDEX IF EXISTS test_index_exists;

DROP INDEX test_index_exists;

-- sequence

DROP SEQUENCE test_sequence_exists;

DROP SEQUENCE IF EXISTS test_sequence_exists;

CREATE SEQUENCE test_sequence_exists;

DROP SEQUENCE IF EXISTS test_sequence_exists;

DROP SEQUENCE test_sequence_exists;

-- schema

DROP SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

CREATE SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

DROP SCHEMA test_schema_exists;

-- type

DROP TYPE test_type_exists;

DROP TYPE IF EXISTS test_type_exists;

CREATE type test_type_exists as (a int, b text);

DROP TYPE IF EXISTS test_type_exists;

DROP TYPE test_type_exists;

-- domain

DROP DOMAIN test_domain_exists;

DROP DOMAIN IF EXISTS test_domain_exists;

CREATE domain test_domain_exists as int not null check (value > 0);

DROP DOMAIN IF EXISTS test_domain_exists;

DROP DOMAIN test_domain_exists;

---
--- role/user/group
---

CREATE USER tu1;
CREATE ROLE tr1;
CREATE GROUP tg1;

DROP USER tu2;

DROP USER IF EXISTS tu1, tu2;

DROP USER tu1;

DROP ROLE tr2;

DROP ROLE IF EXISTS tr1, tr2;

DROP ROLE tr1;

DROP GROUP tg2;

DROP GROUP IF EXISTS tg1, tg2;

DROP GROUP tg1;

-- collation
DROP COLLATION IF EXISTS test_collation_exists;

-- conversion
DROP CONVERSION test_conversion_exists;
DROP CONVERSION IF EXISTS test_conversion_exists;
CREATE CONVERSION test_conversion_exists
    FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
DROP CONVERSION test_conversion_exists;

-- text search parser
DROP TEXT SEARCH PARSER test_tsparser_exists;
DROP TEXT SEARCH PARSER IF EXISTS test_tsparser_exists;

-- text search dictionary
DROP TEXT SEARCH DICTIONARY test_tsdict_exists;
DROP TEXT SEARCH DICTIONARY IF EXISTS test_tsdict_exists;
CREATE TEXT SEARCH DICTIONARY test_tsdict_exists (
        Template=ispell,
        DictFile=ispell_sample,
        AffFile=ispell_sample
);
DROP TEXT SEARCH DICTIONARY test_tsdict_exists;

-- test search template
DROP TEXT SEARCH TEMPLATE test_tstemplate_exists;
DROP TEXT SEARCH TEMPLATE IF EXISTS test_tstemplate_exists;

-- text search configuration
DROP TEXT SEARCH CONFIGURATION test_tsconfig_exists;
DROP TEXT SEARCH CONFIGURATION IF EXISTS test_tsconfig_exists;
CREATE TEXT SEARCH CONFIGURATION test_tsconfig_exists (COPY=english);
DROP TEXT SEARCH CONFIGURATION test_tsconfig_exists;

-- extension
DROP EXTENSION test_extension_exists;
DROP EXTENSION IF EXISTS test_extension_exists;

-- functions
DROP FUNCTION test_function_exists();
DROP FUNCTION IF EXISTS test_function_exists();

DROP FUNCTION test_function_exists(int, text, int[]);
DROP FUNCTION IF EXISTS test_function_exists(int, text, int[]);

-- aggregate
DROP AGGREGATE test_aggregate_exists(*);
DROP AGGREGATE IF EXISTS test_aggregate_exists(*);

DROP AGGREGATE test_aggregate_exists(int);
DROP AGGREGATE IF EXISTS test_aggregate_exists(int);

-- operator
DROP OPERATOR @#@ (int, int);
DROP OPERATOR IF EXISTS @#@ (int, int);
CREATE OPERATOR @#@
        (leftarg = int8, rightarg = int8, procedure = int8xor);
DROP OPERATOR @#@ (int8, int8);

-- language
DROP LANGUAGE test_language_exists;
DROP LANGUAGE IF EXISTS test_language_exists;

-- cast
DROP CAST (text AS text);
DROP CAST IF EXISTS (text AS text);

-- trigger
DROP TRIGGER test_trigger_exists ON test_exists;
DROP TRIGGER IF EXISTS test_trigger_exists ON test_exists;

DROP TRIGGER test_trigger_exists ON no_such_table;
DROP TRIGGER IF EXISTS test_trigger_exists ON no_such_table;

CREATE TRIGGER test_trigger_exists
    BEFORE UPDATE ON test_exists
    FOR EACH ROW EXECUTE PROCEDURE suppress_redundant_updates_trigger();
DROP TRIGGER test_trigger_exists ON test_exists;

-- rule
DROP RULE test_rule_exists ON test_exists;
DROP RULE IF EXISTS test_rule_exists ON test_exists;

DROP RULE test_rule_exists ON no_such_table;
DROP RULE IF EXISTS test_rule_exists ON no_such_table;

CREATE RULE test_rule_exists AS ON INSERT TO test_exists
    DO INSTEAD
    INSERT INTO test_exists VALUES (NEW.a, NEW.b || NEW.a::text);
DROP RULE test_rule_exists ON test_exists;

-- foreign data wrapper
DROP FOREIGN DATA WRAPPER test_fdw_exists;
DROP FOREIGN DATA WRAPPER IF EXISTS test_fdw_exists;

-- foreign server
DROP SERVER test_server_exists;
DROP SERVER IF EXISTS test_server_exists;

-- operator class
DROP OPERATOR CLASS test_operator_class USING btree;
DROP OPERATOR CLASS IF EXISTS test_operator_class USING btree;

DROP OPERATOR CLASS test_operator_class USING no_such_am;
DROP OPERATOR CLASS IF EXISTS test_operator_class USING no_such_am;

-- operator family
DROP OPERATOR FAMILY test_operator_family USING btree;
DROP OPERATOR FAMILY IF EXISTS test_operator_family USING btree;

DROP OPERATOR FAMILY test_operator_family USING no_such_am;
DROP OPERATOR FAMILY IF EXISTS test_operator_family USING no_such_am;

-- drop the table

DROP TABLE IF EXISTS test_exists;

DROP TABLE test_exists;
