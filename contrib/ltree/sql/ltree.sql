CREATE EXTENSION ltree;

-- max length for a label
\set maxlbl 1000

-- Check whether any of our opclasses fail amvalidate
SELECT amname, opcname
FROM pg_opclass opc LEFT JOIN pg_am am ON am.oid = opcmethod
WHERE opc.oid >= 16384 AND NOT amvalidate(opc.oid);

SELECT ''::ltree;
SELECT '1'::ltree;
SELECT '1.2'::ltree;
SELECT '1.2.-3'::ltree;
SELECT '1.2._3'::ltree;

-- empty labels not allowed
SELECT '.2.3'::ltree;
SELECT '1..3'::ltree;
SELECT '1.2.'::ltree;

SELECT repeat('x', :maxlbl)::ltree;
SELECT repeat('x', :maxlbl + 1)::ltree;

SELECT ltree2text('1.2.3.34.sdf');
SELECT text2ltree('1.2.3.34.sdf');

SELECT subltree('Top.Child1.Child2',1,2);
SELECT subpath('Top.Child1.Child2',1,2);
SELECT subpath('Top.Child1.Child2',-1,1);
SELECT subpath('Top.Child1.Child2',0,-2);
SELECT subpath('Top.Child1.Child2',0,-1);
SELECT subpath('Top.Child1.Child2',0,0);
SELECT subpath('Top.Child1.Child2',1,0);
SELECT subpath('Top.Child1.Child2',0);
SELECT subpath('Top.Child1.Child2',1);


SELECT index('1.2.3.4.5.6','1.2');
SELECT index('a.1.2.3.4.5.6','1.2');
SELECT index('a.1.2.3.4.5.6','1.2.3');
SELECT index('a.1.2.3.4.5.6','1.2.3.j');
SELECT index('a.1.2.3.4.5.6','1.2.3.j.4.5.5.5.5.5.5');
SELECT index('a.1.2.3.4.5.6','1.2.3');
SELECT index('a.1.2.3.4.5.6','6');
SELECT index('a.1.2.3.4.5.6','6.1');
SELECT index('a.1.2.3.4.5.6','5.6');
SELECT index('0.1.2.3.5.4.5.6','5.6');
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',3);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',6);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',7);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',-7);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',-4);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',-3);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',-2);
SELECT index('0.1.2.3.5.4.5.6.8.5.6.8','5.6',-20000);


SELECT 'Top.Child1.Child2'::ltree || 'Child3'::text;
SELECT 'Top.Child1.Child2'::ltree || 'Child3'::ltree;
SELECT 'Top_0'::ltree || 'Top.Child1.Child2'::ltree;
SELECT 'Top.Child1.Child2'::ltree || ''::ltree;
SELECT ''::ltree || 'Top.Child1.Child2'::ltree;

SELECT lca('{la.2.3,1.2.3.4.5.6,""}') IS NULL;
SELECT lca('{la.2.3,1.2.3.4.5.6}') IS NULL;
SELECT lca('{1.la.2.3,1.2.3.4.5.6}');
SELECT lca('{1.2.3,1.2.3.4.5.6}');
SELECT lca('{1.2.3}');
SELECT lca('{1}'), lca('{1}') IS NULL;
SELECT lca('{}') IS NULL;
SELECT lca('1.la.2.3','1.2.3.4.5.6');
SELECT lca('1.2.3','1.2.3.4.5.6');
SELECT lca('1.2.2.3','1.2.3.4.5.6');
SELECT lca('1.2.2.3','1.2.3.4.5.6','');
SELECT lca('1.2.2.3','1.2.3.4.5.6','2');
SELECT lca('1.2.2.3','1.2.3.4.5.6','1');


SELECT '1'::lquery;
SELECT '4|3|2'::lquery;
SELECT '1.2'::lquery;
SELECT '1.4|3|2'::lquery;
SELECT '1.0'::lquery;
SELECT '4|3|2.0'::lquery;
SELECT '1.2.0'::lquery;
SELECT '1.4|3|2.0'::lquery;
SELECT '1.*'::lquery;
SELECT '4|3|2.*'::lquery;
SELECT '1.2.*'::lquery;
SELECT '1.4|3|2.*'::lquery;
SELECT '*.1.*'::lquery;
SELECT '*.4|3|2.*'::lquery;
SELECT '*.1.2.*'::lquery;
SELECT '*.1.4|3|2.*'::lquery;
SELECT '1.*.4|3|2'::lquery;
SELECT '1.*.4|3|2.0'::lquery;
SELECT '1.*.4|3|2.*{1,4}'::lquery;
SELECT '1.*.4|3|2.*{,4}'::lquery;
SELECT '1.*.4|3|2.*{1,}'::lquery;
SELECT '1.*.4|3|2.*{1}'::lquery;
SELECT 'foo.bar{,}.!a*|b{1,}.c{,44}.d{3,4}'::lquery;
SELECT 'foo*@@*'::lquery;
SELECT 'qwerty%@*.tu'::lquery;

-- empty labels not allowed
SELECT '.2.3'::lquery;
SELECT '1..3'::lquery;
SELECT '1.2.'::lquery;
SELECT '@.2.3'::lquery;
SELECT '1.@.3'::lquery;
SELECT '1.2.@'::lquery;
SELECT '!.2.3'::lquery;
SELECT '1.!.3'::lquery;
SELECT '1.2.!'::lquery;
SELECT '1.2.3|@.4'::lquery;

SELECT (repeat('x', :maxlbl) || '*@@*')::lquery;
SELECT (repeat('x', :maxlbl + 1) || '*@@*')::lquery;
SELECT ('!' || repeat('x', :maxlbl))::lquery;
SELECT ('!' || repeat('x', :maxlbl + 1))::lquery;

SELECT nlevel('1.2.3.4');
SELECT nlevel(('1' || repeat('.1', 65534))::ltree);
SELECT nlevel(('1' || repeat('.1', 65535))::ltree);
SELECT nlevel(('1' || repeat('.1', 65534))::ltree || '1');
SELECT ('1' || repeat('.1', 65534))::lquery IS NULL;
SELECT ('1' || repeat('.1', 65535))::lquery IS NULL;
SELECT '*{65535}'::lquery;
SELECT '*{65536}'::lquery;
SELECT '*{,65534}'::lquery;
SELECT '*{,65535}'::lquery;
SELECT '*{,65536}'::lquery;
SELECT '*{4,3}'::lquery;

SELECT '1.2'::ltree  < '2.2'::ltree;
SELECT '1.2'::ltree  <= '2.2'::ltree;
SELECT '2.2'::ltree  = '2.2'::ltree;
SELECT '3.2'::ltree  >= '2.2'::ltree;
SELECT '3.2'::ltree  > '2.2'::ltree;

SELECT '1.2.3'::ltree @> '1.2.3.4'::ltree;
SELECT '1.2.3.4'::ltree @> '1.2.3.4'::ltree;
SELECT '1.2.3.4.5'::ltree @> '1.2.3.4'::ltree;
SELECT '1.3.3'::ltree @> '1.2.3.4'::ltree;

SELECT 'a.b.c.d.e'::ltree ~ 'a.b.c.d.e';
SELECT 'a.b.c.d.e'::ltree ~ 'A.b.c.d.e';
SELECT 'a.b.c.d.e'::ltree ~ 'A@.b.c.d.e';
SELECT 'aa.b.c.d.e'::ltree ~ 'A@.b.c.d.e';
SELECT 'aa.b.c.d.e'::ltree ~ 'A*.b.c.d.e';
SELECT 'aa.b.c.d.e'::ltree ~ 'A*@.b.c.d.e';
SELECT 'aa.b.c.d.e'::ltree ~ 'A*@|g.b.c.d.e';
SELECT 'g.b.c.d.e'::ltree ~ 'A*@|g.b.c.d.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.b.c.d.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{3}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{4}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{,4}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,4}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,3}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,3}';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,4}';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2,5}';
SELECT 'a.b.c.d.e'::ltree ~ '*{2,3}.e';
SELECT 'a.b.c.d.e'::ltree ~ '*{2,4}.e';
SELECT 'a.b.c.d.e'::ltree ~ '*{2,5}.e';
SELECT 'a.b.c.d.e'::ltree ~ '*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*.e.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.*.d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!d';
SELECT 'a.b.c.d.e'::ltree ~ '!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '!a.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!e';
SELECT 'a.b.c.d.e'::ltree ~ '*.!e.*';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*.!e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*.!d';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*.!f.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.*.!f.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.*.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.!d';
SELECT 'a.b.c.d.e'::ltree ~ 'a.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.a.*.!d.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*.c.*';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*.c.*';
SELECT 'a.b.c.d.e'::ltree ~ '!b.b.*';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '!b.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*{2}.!b.*.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*{1}.!b.*.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*{1}.!b.*{1}.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.!b.*{1}.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*{1}.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*{1}.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*.!c.*.e';
SELECT 'a.b.c.d.e'::ltree ~ '!b.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*{2}.!b.*.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*{1}.!b.*.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*{1}.!b.*{1}.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ 'a.!b.*{1}.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '!b.*{1}.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*{1}.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ '*.!b.*.!c.*';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{2}.*{2}';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{1}.*{2}.e';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{1}.*{4}';
SELECT 'a.b.c.d.e'::ltree ~ 'a.*{5}.*';
SELECT '5.0.1.0'::ltree ~ '5.!0.!0.0';
SELECT 'a.b'::ltree ~ '!a.!a';

SELECT 'a.b.c.d.e'::ltree ~ 'a{,}';
SELECT 'a.b.c.d.e'::ltree ~ 'a{1,}.*';
SELECT 'a.b.c.d.e'::ltree ~ 'a{,}.!a{,}';
SELECT 'a.b.c.d.a'::ltree ~ 'a{,}.!a{,}';
SELECT 'a.b.c.d.a'::ltree ~ 'a{,2}.!a{1,}';
SELECT 'a.b.c.d.e'::ltree ~ 'a{,2}.!a{1,}';
SELECT 'a.b.c.d.e'::ltree ~ '!x{,}';
SELECT 'a.b.c.d.e'::ltree ~ '!c{,}';
SELECT 'a.b.c.d.e'::ltree ~ '!c{0,3}.!a{2,}';
SELECT 'a.b.c.d.e'::ltree ~ '!c{0,3}.!d{2,}.*';

SELECT 'QWER_TY'::ltree ~ 'q%@*';
SELECT 'QWER_TY'::ltree ~ 'q%@*%@*';
SELECT 'QWER_TY'::ltree ~ 'Q_t%@*';
SELECT 'QWER_GY'::ltree ~ 'q_t%@*';

--ltxtquery
SELECT '!tree & aWdf@*'::ltxtquery;
SELECT 'tree & aw_qw%*'::ltxtquery;
SELECT 'tree & aw-qw%*'::ltxtquery;

SELECT 'ltree.awdfg'::ltree @ '!tree & aWdf@*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ '!tree & aWdf@*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ '!tree | aWdf@*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree | aWdf@*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & aWdf@*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & aWdf@'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & aWdf*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & aWdf'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & awdf*'::ltxtquery;
SELECT 'tree.awdfg'::ltree @ 'tree & aWdfg@'::ltxtquery;
SELECT 'tree.awdfg_qwerty'::ltree @ 'tree & aw_qw%*'::ltxtquery;
SELECT 'tree.awdfg_qwerty'::ltree @ 'tree & aw_rw%*'::ltxtquery;

--arrays

SELECT '{1.2.3}'::ltree[] @> '1.2.3.4';
SELECT '{1.2.3.4}'::ltree[] @> '1.2.3.4';
SELECT '{1.2.3.4.5}'::ltree[] @> '1.2.3.4';
SELECT '{1.3.3}'::ltree[] @> '1.2.3.4';
SELECT '{5.67.8, 1.2.3}'::ltree[] @> '1.2.3.4';
SELECT '{5.67.8, 1.2.3.4}'::ltree[] @> '1.2.3.4';
SELECT '{5.67.8, 1.2.3.4.5}'::ltree[] @> '1.2.3.4';
SELECT '{5.67.8, 1.3.3}'::ltree[] @> '1.2.3.4';
SELECT '{1.2.3, 7.12.asd}'::ltree[] @> '1.2.3.4';
SELECT '{1.2.3.4, 7.12.asd}'::ltree[] @> '1.2.3.4';
SELECT '{1.2.3.4.5, 7.12.asd}'::ltree[] @> '1.2.3.4';
SELECT '{1.3.3, 7.12.asd}'::ltree[] @> '1.2.3.4';
SELECT '{ltree.asd, tree.awdfg}'::ltree[] @ 'tree & aWdfg@'::ltxtquery;
SELECT '{j.k.l.m, g.b.c.d.e}'::ltree[] ~ 'A*@|g.b.c.d.e';
SELECT 'a.b.c.d.e'::ltree ? '{A.b.c.d.e}';
SELECT 'a.b.c.d.e'::ltree ? '{a.b.c.d.e}';
SELECT 'a.b.c.d.e'::ltree ? '{A.b.c.d.e, a.*}';
SELECT '{a.b.c.d.e,B.df}'::ltree[] ? '{A.b.c.d.e}';
SELECT '{a.b.c.d.e,B.df}'::ltree[] ? '{A.b.c.d.e,*.df}';

--extractors
SELECT ('{3456,1.2.3.34}'::ltree[] ?@> '1.2.3.4') is null;
SELECT '{3456,1.2.3}'::ltree[] ?@> '1.2.3.4';
SELECT '{3456,1.2.3.4}'::ltree[] ?<@ '1.2.3';
SELECT ('{3456,1.2.3.4}'::ltree[] ?<@ '1.2.5') is null;
SELECT '{ltree.asd, tree.awdfg}'::ltree[] ?@ 'tree & aWdfg@'::ltxtquery;
SELECT '{j.k.l.m, g.b.c.d.e}'::ltree[] ?~ 'A*@|g.b.c.d.e';

-- Check that the hash_ltree() and hash_ltree_extended() function's lower
-- 32 bits match when the seed is 0 and do not match when the seed != 0
SELECT v as value, hash_ltree(v)::bit(32) as standard,
       hash_ltree_extended(v, 0)::bit(32) as extended0,
       hash_ltree_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::ltree), (''::ltree), ('0'::ltree), ('0.1'::ltree),
       ('0.1.2'::ltree), ('0'::ltree), ('0_asd.1_ASD'::ltree)) x(v)
WHERE  hash_ltree(v)::bit(32) != hash_ltree_extended(v, 0)::bit(32)
       OR hash_ltree(v)::bit(32) = hash_ltree_extended(v, 1)::bit(32);

CREATE TABLE ltreetest (t ltree);
\copy ltreetest FROM 'data/ltree.data'

SELECT count(*) from ltreetest;

SELECT * FROM ltreetest WHERE t <  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t <= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t =  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t @> '1.1.1' order by t asc;
SELECT * FROM ltreetest WHERE t <@ '1.1.1' order by t asc;
SELECT * FROM ltreetest WHERE t @ '23 & 1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '1.1.1.*' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '*.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*{1}.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*.2' order by t asc;
SELECT * FROM ltreetest WHERE t ? '{23.*.1,23.*.2}' order by t asc;

create unique index tstidx on ltreetest (t);
set enable_seqscan=off;

SELECT * FROM ltreetest WHERE t <  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t <= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t =  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >  '12.3' order by t asc;

drop index tstidx;
create index tstidx on ltreetest using gist (t);
set enable_seqscan=off;

SELECT * FROM ltreetest WHERE t <  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t <= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t =  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >= '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t >  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t @> '1.1.1' order by t asc;
SELECT * FROM ltreetest WHERE t <@ '1.1.1' order by t asc;
SELECT * FROM ltreetest WHERE t @ '23 & 1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '1.1.1.*' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '*.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*{1}.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*.1' order by t asc;
SELECT * FROM ltreetest WHERE t ~ '23.*.2' order by t asc;
SELECT * FROM ltreetest WHERE t ? '{23.*.1,23.*.2}' order by t asc;

drop index tstidx;

--- test hash index

create index tstidx on ltreetest using hash (t);
set enable_seqscan=off;
set enable_bitmapscan=off;

EXPLAIN (COSTS OFF)
SELECT * FROM ltreetest WHERE t =  '12.3' order by t asc;
SELECT * FROM ltreetest WHERE t =  '12.3' order by t asc;

reset enable_seqscan;
reset enable_bitmapscan;

-- test hash aggregate

set enable_hashagg=on;
set enable_sort=off;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM (
SELECT t FROM (SELECT * FROM ltreetest UNION ALL SELECT * FROM ltreetest) t1 GROUP BY t
) t2;

SELECT count(*) FROM (
SELECT t FROM (SELECT * FROM ltreetest UNION ALL SELECT * FROM ltreetest) t1 GROUP BY t
) t2;

reset enable_hashagg;
reset enable_sort;

drop index tstidx;

-- test gist index

create index tstidx on ltreetest using gist (t gist_ltree_ops(siglen=0));
create index tstidx on ltreetest using gist (t gist_ltree_ops(siglen=2025));
create index tstidx on ltreetest using gist (t gist_ltree_ops(siglen=2028));
create index tstidx on ltreetest using gist (t gist_ltree_ops(siglen=2019));
create index tstidx on ltreetest using gist (t gist_ltree_ops(siglen=2024));

SELECT count(*) FROM ltreetest WHERE t <  '12.3';
SELECT count(*) FROM ltreetest WHERE t <= '12.3';
SELECT count(*) FROM ltreetest WHERE t =  '12.3';
SELECT count(*) FROM ltreetest WHERE t >= '12.3';
SELECT count(*) FROM ltreetest WHERE t >  '12.3';
SELECT count(*) FROM ltreetest WHERE t @> '1.1.1';
SELECT count(*) FROM ltreetest WHERE t <@ '1.1.1';
SELECT count(*) FROM ltreetest WHERE t @ '23 & 1';
SELECT count(*) FROM ltreetest WHERE t ~ '1.1.1.*';
SELECT count(*) FROM ltreetest WHERE t ~ '*.1';
SELECT count(*) FROM ltreetest WHERE t ~ '23.*{1}.1';
SELECT count(*) FROM ltreetest WHERE t ~ '23.*.1';
SELECT count(*) FROM ltreetest WHERE t ~ '23.*.2';
SELECT count(*) FROM ltreetest WHERE t ? '{23.*.1,23.*.2}';

create table _ltreetest (t ltree[]);
\copy _ltreetest FROM 'data/_ltree.data'

SELECT count(*) FROM _ltreetest WHERE t @> '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t <@ '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t @ '23 & 1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '1.1.1.*' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*{1}.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.2' ;
SELECT count(*) FROM _ltreetest WHERE t ? '{23.*.1,23.*.2}' ;

create index _tstidx on _ltreetest using gist (t);
set enable_seqscan=off;

SELECT count(*) FROM _ltreetest WHERE t @> '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t <@ '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t @ '23 & 1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '1.1.1.*' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*{1}.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.2' ;
SELECT count(*) FROM _ltreetest WHERE t ? '{23.*.1,23.*.2}' ;

drop index _tstidx;
create index _tstidx on _ltreetest using gist (t gist__ltree_ops(siglen=0));
create index _tstidx on _ltreetest using gist (t gist__ltree_ops(siglen=2025));
create index _tstidx on _ltreetest using gist (t gist__ltree_ops(siglen=2024));

SELECT count(*) FROM _ltreetest WHERE t @> '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t <@ '1.1.1' ;
SELECT count(*) FROM _ltreetest WHERE t @ '23 & 1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '1.1.1.*' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*{1}.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.1' ;
SELECT count(*) FROM _ltreetest WHERE t ~ '23.*.2' ;
SELECT count(*) FROM _ltreetest WHERE t ? '{23.*.1,23.*.2}' ;

-- test non-error-throwing input

SELECT str as "value", typ as "type",
       pg_input_is_valid(str,typ) as ok,
       errinfo.sql_error_code,
       errinfo.message,
       errinfo.detail,
       errinfo.hint
FROM (VALUES ('.2.3', 'ltree'),
             ('1.2.', 'ltree'),
             ('1.2.3','ltree'),
             ('@.2.3','lquery'),
             (' 2.3', 'lquery'),
             ('1.2.3','lquery'),
             ('$tree & aWdf@*','ltxtquery'),
             ('!tree & aWdf@*','ltxtquery'))
      AS a(str,typ),
     LATERAL pg_input_error_info(a.str, a.typ) as errinfo;
