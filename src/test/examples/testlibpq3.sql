CREATE SCHEMA testlibpq3;
SET search_path = testlibpq3;
SET standard_conforming_strings = ON;
CREATE TABLE test1 (i int4, t text, b bytea);
INSERT INTO test1 values (1, 'joe''s place', '\000\001\002\003\004');
INSERT INTO test1 values (2, 'ho there', '\004\003\002\001\000');
