CREATE EXTENSION unaccent;

-- must have a UTF8 database
SELECT getdatabaseencoding();

SET client_encoding TO 'UTF8';

SELECT unaccent('foobar');
SELECT unaccent('ёлка');
SELECT unaccent('ЁЖИК');
SELECT unaccent('˃˖˗˜');
SELECT unaccent('À');  -- Remove combining diacritical 0x0300

SELECT unaccent('unaccent', 'foobar');
SELECT unaccent('unaccent', 'ёлка');
SELECT unaccent('unaccent', 'ЁЖИК');
SELECT unaccent('unaccent', '˃˖˗˜');
SELECT unaccent('unaccent', 'À');

SELECT ts_lexize('unaccent', 'foobar');
SELECT ts_lexize('unaccent', 'ёлка');
SELECT ts_lexize('unaccent', 'ЁЖИК');
SELECT ts_lexize('unaccent', '˃˖˗˜');
SELECT ts_lexize('unaccent', 'À');
