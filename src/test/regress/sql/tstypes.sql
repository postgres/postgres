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
select '''w'':4A,3B,2C,1D,5 a:8';
select 'a:3A b:2a'::tsvector || 'ba:1234 a:1B';
select setweight('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd zxc:81,567,222A'::tsvector, 'c');
select strip('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd'::tsvector);

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

select 'a' < 'b & c'::tsquery as "true";
select 'a' > 'b & c'::tsquery as "false";
select 'a | f' < 'b & c'::tsquery as "true";
select 'a | ff' < 'b & c'::tsquery as "false";
select 'a | f | g' < 'b & c'::tsquery as "false";

select numnode( 'new'::tsquery );
select numnode( 'new & york'::tsquery );
select numnode( 'new & york | qwery'::tsquery );

select 'foo & bar'::tsquery && 'asd';
select 'foo & bar'::tsquery || 'asd & fg';
select 'foo & bar'::tsquery || !!'asd & fg'::tsquery;
select 'foo & bar'::tsquery && 'asd | fg';

-- tsvector-tsquery operations

select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca' as "true";
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:B' as "true";
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:A' as "true";
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:C' as "false";
select 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & ca:CB' as "true";

select ts_rank(' a:1 s:2C d g'::tsvector, 'a | s');
select ts_rank(' a:1 s:2B d g'::tsvector, 'a | s');
select ts_rank(' a:1 s:2 d g'::tsvector, 'a | s');
select ts_rank(' a:1 s:2C d g'::tsvector, 'a & s');
select ts_rank(' a:1 s:2B d g'::tsvector, 'a & s');
select ts_rank(' a:1 s:2 d g'::tsvector, 'a & s');

select ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a | s');
select ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a | s');
select ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a | s');
select ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a & s');
select ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a & s');
select ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a & s');

