/* contrib/test_parser/test_parser--unpackaged--1.0.sql */

ALTER EXTENSION test_parser ADD function testprs_start(internal,integer);
ALTER EXTENSION test_parser ADD function testprs_getlexeme(internal,internal,internal);
ALTER EXTENSION test_parser ADD function testprs_end(internal);
ALTER EXTENSION test_parser ADD function testprs_lextype(internal);
ALTER EXTENSION test_parser ADD text search parser testparser;
