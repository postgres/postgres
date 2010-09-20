/* contrib/test_parser/uninstall_test_parser.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP TEXT SEARCH PARSER testparser;

DROP FUNCTION testprs_start(internal, int4);

DROP FUNCTION testprs_getlexeme(internal, internal, internal);

DROP FUNCTION testprs_end(internal);

DROP FUNCTION testprs_lextype(internal);
