--Base tsvector test

SELECT '1'::tsvector;
SELECT '1 '::tsvector;
SELECT ' 1'::tsvector;
SELECT ' 1 '::tsvector;
SELECT '1 2'::tsvector;
SELECT '''1 2'''::tsvector;
SELECT E'''1 \\''2'''::tsvector;
SELECT E'''1 \\''2''3'::tsvector;
SELECT E'''1 \\''2'' 3'::tsvector;
SELECT E'''1 \\''2'' '' 3'' 4 '::tsvector;
SELECT $$'\\as' ab\c ab\\c AB\\\c ab\\\\c$$::tsvector;
SELECT tsvectorin(tsvectorout($$'\\as' ab\c ab\\c AB\\\c ab\\\\c$$::tsvector));
SELECT '''w'':4A,3B,2C,1D,5 a:8';
SELECT 'a:3A b:2a'::tsvector || 'ba:1234 a:1B';
SELECT setweight('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd zxc:81,567,222A'::tsvector, 'c');
SELECT strip('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd'::tsvector);

--Base tsquery test
SELECT '1'::tsquery;
SELECT '1 '::tsquery;
SELECT ' 1'::tsquery;
SELECT ' 1 '::tsquery;
SELECT '''1 2'''::tsquery;
SELECT E'''1 \\''2'''::tsquery;
SELECT '!1'::tsquery;
SELECT '1|2'::tsquery;
SELECT '1|!2'::tsquery;
SELECT '!1|2'::tsquery;
SELECT '!1|!2'::tsquery;
SELECT '!(!1|!2)'::tsquery;
SELECT '!(!1|2)'::tsquery;
SELECT '!(1|!2)'::tsquery;
SELECT '!(1|2)'::tsquery;
SELECT '1&2'::tsquery;
SELECT '!1&2'::tsquery;
SELECT '1&!2'::tsquery;
SELECT '!1&!2'::tsquery;
SELECT '(1&2)'::tsquery;
SELECT '1&(2)'::tsquery;
SELECT '!(1)&2'::tsquery;
SELECT '!(1&2)'::tsquery;
SELECT '1|2&3'::tsquery;
SELECT '1|(2&3)'::tsquery;
SELECT '(1|2)&3'::tsquery;
SELECT '1|2&!3'::tsquery;
SELECT '1|!2&3'::tsquery;
SELECT '!1|2&3'::tsquery;
SELECT '!1|(2&3)'::tsquery;
SELECT '!(1|2)&3'::tsquery;
SELECT '(!1|2)&3'::tsquery;
SELECT '1|(2|(4|(5|6)))'::tsquery;
SELECT '1|2|4|5|6'::tsquery;
SELECT '1&(2&(4&(5&6)))'::tsquery;
SELECT '1&2&4&5&6'::tsquery;
SELECT '1&(2&(4&(5|6)))'::tsquery;
SELECT '1&(2&(4&(5|!6)))'::tsquery;
SELECT E'1&(''2''&('' 4''&(\\|5 | ''6 \\'' !|&'')))'::tsquery;
SELECT $$'\\as'$$::tsquery;

SELECT 'a' < 'b & c'::tsquery as "true";
SELECT 'a' > 'b & c'::tsquery as "false";
SELECT 'a | f' < 'b & c'::tsquery as "true";
SELECT 'a | ff' < 'b & c'::tsquery as "false";
SELECT 'a | f | g' < 'b & c'::tsquery as "false";

SELECT numnode( 'new'::tsquery );
SELECT numnode( 'new & york'::tsquery );
SELECT numnode( 'new & york | qwery'::tsquery );

SELECT 'foo & bar'::tsquery && 'asd';
SELECT 'foo & bar'::tsquery || 'asd & fg';
SELECT 'foo & bar'::tsquery || !!'asd & fg'::tsquery;
SELECT 'foo & bar'::tsquery && 'asd | fg';

-- tsvector-tsquery operations

SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca' as "true";
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:B' as "true";
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:A' as "true";
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:C' as "false";
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:CB' as "true";

SELECT ts_rank(' a:1 s:2C d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 s:2B d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 s:2 d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 s:2C d g'::tsvector, 'a & s');
SELECT ts_rank(' a:1 s:2B d g'::tsvector, 'a & s');
SELECT ts_rank(' a:1 s:2 d g'::tsvector, 'a & s');

SELECT ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a & s');
SELECT ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a & s');
SELECT ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a & s');
