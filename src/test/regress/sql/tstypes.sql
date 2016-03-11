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
SELECT 'a:* & nbb:*ac | doo:a* | goo'::tsquery;

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
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & c:*C' as "false";
SELECT 'a b:89  ca:23A,64b d:34c'::tsvector @@ 'd:AC & c:*CB' as "true";
SELECT 'a b:89  ca:23A,64b cb:80c d:34c'::tsvector @@ 'd:AC & c:*C' as "true";
SELECT 'a b:89  ca:23A,64c cb:80b d:34c'::tsvector @@ 'd:AC & c:*C' as "true";
SELECT 'a b:89  ca:23A,64c cb:80b d:34c'::tsvector @@ 'd:AC & c:*B' as "true";

SELECT 'supernova'::tsvector @@ 'super'::tsquery AS "false";
SELECT 'supeanova supernova'::tsvector @@ 'super'::tsquery AS "false";
SELECT 'supeznova supernova'::tsvector @@ 'super'::tsquery AS "false";
SELECT 'supernova'::tsvector @@ 'super:*'::tsquery AS "true";
SELECT 'supeanova supernova'::tsvector @@ 'super:*'::tsquery AS "true";
SELECT 'supeznova supernova'::tsvector @@ 'super:*'::tsquery AS "true";

SELECT ts_rank(' a:1 s:2C d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 sa:2C d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 sa:2C d g'::tsvector, 'a | s:*');
SELECT ts_rank(' a:1 sa:2C d g'::tsvector, 'a | sa:*');
SELECT ts_rank(' a:1 s:2B d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 s:2 d g'::tsvector, 'a | s');
SELECT ts_rank(' a:1 s:2C d g'::tsvector, 'a & s');
SELECT ts_rank(' a:1 s:2B d g'::tsvector, 'a & s');
SELECT ts_rank(' a:1 s:2 d g'::tsvector, 'a & s');

SELECT ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 sa:2C d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 sa:2C d g'::tsvector, 'a | s:*');
SELECT ts_rank_cd(' a:1 sa:2C d g'::tsvector, 'a | sa:*');
SELECT ts_rank_cd(' a:1 sa:3C sab:2c d g'::tsvector, 'a | sa:*');
SELECT ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a | s');
SELECT ts_rank_cd(' a:1 s:2C d g'::tsvector, 'a & s');
SELECT ts_rank_cd(' a:1 s:2B d g'::tsvector, 'a & s');
SELECT ts_rank_cd(' a:1 s:2 d g'::tsvector, 'a & s');

-- tsvector editing operations

SELECT strip('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd'::tsvector);
SELECT strip('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector);
SELECT strip('base hidden rebel spaceship strike'::tsvector);

SELECT delete(to_tsvector('english', 'Rebel spaceships, striking from a hidden base'), 'spaceship');
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, 'base');
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, 'bas');
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, 'bases');
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, 'spaceship');
SELECT delete('base hidden rebel spaceship strike'::tsvector, 'spaceship');

SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, ARRAY['spaceship','rebel']);
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, ARRAY['spaceships','rebel']);
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, ARRAY['spaceshi','rebel']);
SELECT delete('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector, ARRAY['spaceship','leya','rebel']);
SELECT delete('base hidden rebel spaceship strike'::tsvector, ARRAY['spaceship','leya','rebel']);
SELECT delete('base hidden rebel spaceship strike'::tsvector, ARRAY['spaceship','leya','rebel', NULL]);

SELECT unnest('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector);
SELECT unnest('base hidden rebel spaceship strike'::tsvector);
SELECT * FROM unnest('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector);
SELECT * FROM unnest('base hidden rebel spaceship strike'::tsvector);
SELECT lexeme, positions[1] from unnest('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector);

SELECT tsvector_to_array('base:7 hidden:6 rebel:1 spaceship:2,33A,34B,35C,36D strike:3'::tsvector);
SELECT tsvector_to_array('base hidden rebel spaceship strike'::tsvector);

SELECT array_to_tsvector(ARRAY['base','hidden','rebel','spaceship','strike']);
SELECT array_to_tsvector(ARRAY['base','hidden','rebel','spaceship', NULL]);

SELECT setweight('w:12B w:13* w:12,5,6 a:1,3* a:3 w asd:1dc asd zxc:81,567,222A'::tsvector, 'c');
SELECT setweight('a:1,3A asd:1C w:5,6,12B,13A zxc:81,222A,567'::tsvector, 'c');
SELECT setweight('a:1,3A asd:1C w:5,6,12B,13A zxc:81,222A,567'::tsvector, 'c', '{a}');
SELECT setweight('a:1,3A asd:1C w:5,6,12B,13A zxc:81,222A,567'::tsvector, 'c', '{a}');
SELECT setweight('a:1,3A asd:1C w:5,6,12B,13A zxc:81,222A,567'::tsvector, 'c', '{a,zxc}');
SELECT setweight('a asd w:5,6,12B,13A zxc'::tsvector, 'c', '{a,zxc}');
SELECT setweight('a asd w:5,6,12B,13A zxc'::tsvector, 'c', ARRAY['a', 'zxc', NULL]);

SELECT filter('base:7A empir:17 evil:15 first:11 galact:16 hidden:6A rebel:1A spaceship:2A strike:3A victori:12 won:9'::tsvector, '{a}');
SELECT filter('base hidden rebel spaceship strike'::tsvector, '{a}');
SELECT filter('base hidden rebel spaceship strike'::tsvector, '{a,b,NULL}');

