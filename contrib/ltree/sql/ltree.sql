\set ECHO none
\i ltree.sql
\set ECHO all

SELECT ''::ltree;
SELECT '1'::ltree;
SELECT '1.2'::ltree;
SELECT '1.2._3'::ltree;

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

SELECT lca('{la.2.3,1.2.3.4.5.6,}') IS NULL;
SELECT lca('{la.2.3,1.2.3.4.5.6}') IS NULL;
SELECT lca('{1.la.2.3,1.2.3.4.5.6}');
SELECT lca('{1.2.3,1.2.3.4.5.6}');
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
SELECT 'qwerty%@*.tu'::lquery;

SELECT nlevel('1.2.3.4');
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


SELECT 'QWER_TY'::ltree ~ 'q%@*';
SELECT 'QWER_TY'::ltree ~ 'Q_t%@*';
SELECT 'QWER_GY'::ltree ~ 'q_t%@*';

--ltxtquery
SELECT '!tree & aWdf@*'::ltxtquery;
SELECT 'tree & aw_qw%*'::ltxtquery;
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

--exractors
SELECT ('{3456,1.2.3.34}'::ltree[] ?@> '1.2.3.4') is null;
SELECT '{3456,1.2.3}'::ltree[] ?@> '1.2.3.4';
SELECT '{3456,1.2.3.4}'::ltree[] ?<@ '1.2.3';
SELECT ('{3456,1.2.3.4}'::ltree[] ?<@ '1.2.5') is null;
SELECT '{ltree.asd, tree.awdfg}'::ltree[] ?@ 'tree & aWdfg@'::ltxtquery;
SELECT '{j.k.l.m, g.b.c.d.e}'::ltree[] ?~ 'A*@|g.b.c.d.e';

CREATE TABLE ltreetest (t ltree);
\copy ltreetest FROM 'data/ltree.data'

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

