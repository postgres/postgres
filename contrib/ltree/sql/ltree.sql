\set ECHO none
\i ltree.sql
\set ECHO all

select ''::ltree;
select '1'::ltree;
select '1.2'::ltree;
select '1.2._3'::ltree;

select subltree('Top.Child1.Child2',1,2);
select subpath('Top.Child1.Child2',1,2);
select subpath('Top.Child1.Child2',-1,1);
select subpath('Top.Child1.Child2',0,-2);
select subpath('Top.Child1.Child2',0,-1);
select subpath('Top.Child1.Child2',0,0);
select subpath('Top.Child1.Child2',1,0);
select subpath('Top.Child1.Child2',0);
select subpath('Top.Child1.Child2',1);

select 'Top.Child1.Child2'::ltree || 'Child3'::text;
select 'Top.Child1.Child2'::ltree || 'Child3'::ltree;
select 'Top_0'::ltree || 'Top.Child1.Child2'::ltree;
select 'Top.Child1.Child2'::ltree || ''::ltree;
select ''::ltree || 'Top.Child1.Child2'::ltree;

select lca('{la.2.3,1.2.3.4.5.6,}') is null;
select lca('{la.2.3,1.2.3.4.5.6}') is null;
select lca('{1.la.2.3,1.2.3.4.5.6}');
select lca('{1.2.3,1.2.3.4.5.6}');
select lca('1.la.2.3','1.2.3.4.5.6');
select lca('1.2.3','1.2.3.4.5.6');
select lca('1.2.2.3','1.2.3.4.5.6');
select lca('1.2.2.3','1.2.3.4.5.6','');
select lca('1.2.2.3','1.2.3.4.5.6','2');
select lca('1.2.2.3','1.2.3.4.5.6','1');


select '1'::lquery;
select '4|3|2'::lquery;
select '1.2'::lquery;
select '1.4|3|2'::lquery;
select '1.0'::lquery;
select '4|3|2.0'::lquery;
select '1.2.0'::lquery;
select '1.4|3|2.0'::lquery;
select '1.*'::lquery;
select '4|3|2.*'::lquery;
select '1.2.*'::lquery;
select '1.4|3|2.*'::lquery;
select '*.1.*'::lquery;
select '*.4|3|2.*'::lquery;
select '*.1.2.*'::lquery;
select '*.1.4|3|2.*'::lquery;
select '1.*.4|3|2'::lquery;
select '1.*.4|3|2.0'::lquery;
select '1.*.4|3|2.*{1,4}'::lquery;
select '1.*.4|3|2.*{,4}'::lquery;
select '1.*.4|3|2.*{1,}'::lquery;
select '1.*.4|3|2.*{1}'::lquery;
select 'qwerty%@*.tu'::lquery;

select nlevel('1.2.3.4');
select '1.2'::ltree  < '2.2'::ltree;
select '1.2'::ltree  <= '2.2'::ltree;
select '2.2'::ltree  = '2.2'::ltree;
select '3.2'::ltree  >= '2.2'::ltree;
select '3.2'::ltree  > '2.2'::ltree;

select '1.2.3'::ltree @> '1.2.3.4'::ltree;
select '1.2.3.4'::ltree @> '1.2.3.4'::ltree;
select '1.2.3.4.5'::ltree @> '1.2.3.4'::ltree;
select '1.3.3'::ltree @> '1.2.3.4'::ltree;

select 'a.b.c.d.e'::ltree ~ 'a.b.c.d.e';
select 'a.b.c.d.e'::ltree ~ 'A.b.c.d.e';
select 'a.b.c.d.e'::ltree ~ 'A@.b.c.d.e';
select 'aa.b.c.d.e'::ltree ~ 'A@.b.c.d.e';
select 'aa.b.c.d.e'::ltree ~ 'A*.b.c.d.e';
select 'aa.b.c.d.e'::ltree ~ 'A*@.b.c.d.e';
select 'aa.b.c.d.e'::ltree ~ 'A*@|g.b.c.d.e';
select 'g.b.c.d.e'::ltree ~ 'A*@|g.b.c.d.e';
select 'a.b.c.d.e'::ltree ~ 'a.b.c.d.e';
select 'a.b.c.d.e'::ltree ~ 'a.*.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{3}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{2}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{4}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{,4}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,4}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,3}.e';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,3}';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,4}';
select 'a.b.c.d.e'::ltree ~ 'a.*{2,5}';
select 'a.b.c.d.e'::ltree ~ '*{2,3}.e';
select 'a.b.c.d.e'::ltree ~ '*{2,4}.e';
select 'a.b.c.d.e'::ltree ~ '*{2,5}.e';
select 'a.b.c.d.e'::ltree ~ '*.e';
select 'a.b.c.d.e'::ltree ~ '*.e.*';
select 'a.b.c.d.e'::ltree ~ '*.d.*';
select 'a.b.c.d.e'::ltree ~ '*.a.*.d.*';
select 'a.b.c.d.e'::ltree ~ '*.!d.*';
select 'a.b.c.d.e'::ltree ~ '*.!d';
select 'a.b.c.d.e'::ltree ~ '!d.*';
select 'a.b.c.d.e'::ltree ~ '!a.*';
select 'a.b.c.d.e'::ltree ~ '*.!e';
select 'a.b.c.d.e'::ltree ~ '*.!e.*';
select 'a.b.c.d.e'::ltree ~ 'a.*.!e';
select 'a.b.c.d.e'::ltree ~ 'a.*.!d';
select 'a.b.c.d.e'::ltree ~ 'a.*.!d.*';
select 'a.b.c.d.e'::ltree ~ 'a.*.!f.*';
select 'a.b.c.d.e'::ltree ~ '*.a.*.!f.*';
select 'a.b.c.d.e'::ltree ~ '*.a.*.!d.*';
select 'a.b.c.d.e'::ltree ~ '*.a.!d.*';
select 'a.b.c.d.e'::ltree ~ '*.a.!d';
select 'a.b.c.d.e'::ltree ~ 'a.!d.*';
select 'a.b.c.d.e'::ltree ~ '*.a.*.!d.*';
select 'a.b.c.d.e'::ltree ~ '*.!b.*';
select 'a.b.c.d.e'::ltree ~ '*.!b.c.*';
select 'a.b.c.d.e'::ltree ~ '*.!b.*.c.*';
select 'a.b.c.d.e'::ltree ~ '!b.*.c.*';
select 'a.b.c.d.e'::ltree ~ '!b.b.*';
select 'a.b.c.d.e'::ltree ~ '!b.*.e';
select 'a.b.c.d.e'::ltree ~ '!b.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '!b.*.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '*{2}.!b.*.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '*{1}.!b.*.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '*{1}.!b.*{1}.!c.*.e';
select 'a.b.c.d.e'::ltree ~ 'a.!b.*{1}.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '!b.*{1}.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '*.!b.*{1}.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '*.!b.*.!c.*.e';
select 'a.b.c.d.e'::ltree ~ '!b.!c.*';
select 'a.b.c.d.e'::ltree ~ '!b.*.!c.*';
select 'a.b.c.d.e'::ltree ~ '*{2}.!b.*.!c.*';
select 'a.b.c.d.e'::ltree ~ '*{1}.!b.*.!c.*';
select 'a.b.c.d.e'::ltree ~ '*{1}.!b.*{1}.!c.*';
select 'a.b.c.d.e'::ltree ~ 'a.!b.*{1}.!c.*';
select 'a.b.c.d.e'::ltree ~ '!b.*{1}.!c.*';
select 'a.b.c.d.e'::ltree ~ '*.!b.*{1}.!c.*';
select 'a.b.c.d.e'::ltree ~ '*.!b.*.!c.*';

select 'QWER_TY'::ltree ~ 'q%@*';
select 'QWER_TY'::ltree ~ 'Q_t%@*';
select 'QWER_GY'::ltree ~ 'q_t%@*';

--ltxtquery
select '!tree & aWdf@*'::ltxtquery;
select 'tree & aw_qw%*'::ltxtquery;
select 'ltree.awdfg'::ltree @ '!tree & aWdf@*'::ltxtquery;
select 'tree.awdfg'::ltree @ '!tree & aWdf@*'::ltxtquery;
select 'tree.awdfg'::ltree @ '!tree | aWdf@*'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree | aWdf@*'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & aWdf@*'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & aWdf@'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & aWdf*'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & aWdf'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & awdf*'::ltxtquery;
select 'tree.awdfg'::ltree @ 'tree & aWdfg@'::ltxtquery;
select 'tree.awdfg_qwerty'::ltree @ 'tree & aw_qw%*'::ltxtquery;
select 'tree.awdfg_qwerty'::ltree @ 'tree & aw_rw%*'::ltxtquery;

--arrays

select '{1.2.3}'::ltree[] @> '1.2.3.4';
select '{1.2.3.4}'::ltree[] @> '1.2.3.4';
select '{1.2.3.4.5}'::ltree[] @> '1.2.3.4';
select '{1.3.3}'::ltree[] @> '1.2.3.4';
select '{5.67.8, 1.2.3}'::ltree[] @> '1.2.3.4';
select '{5.67.8, 1.2.3.4}'::ltree[] @> '1.2.3.4';
select '{5.67.8, 1.2.3.4.5}'::ltree[] @> '1.2.3.4';
select '{5.67.8, 1.3.3}'::ltree[] @> '1.2.3.4';
select '{1.2.3, 7.12.asd}'::ltree[] @> '1.2.3.4';
select '{1.2.3.4, 7.12.asd}'::ltree[] @> '1.2.3.4';
select '{1.2.3.4.5, 7.12.asd}'::ltree[] @> '1.2.3.4';
select '{1.3.3, 7.12.asd}'::ltree[] @> '1.2.3.4';
select '{ltree.asd, tree.awdfg}'::ltree[] @ 'tree & aWdfg@'::ltxtquery;
select '{j.k.l.m, g.b.c.d.e}'::ltree[] ~ 'A*@|g.b.c.d.e';

--exractors
select ('{3456,1.2.3.34}'::ltree[] ?@> '1.2.3.4') is null;
select '{3456,1.2.3}'::ltree[] ?@> '1.2.3.4';
select '{3456,1.2.3.4}'::ltree[] ?<@ '1.2.3';
select ('{3456,1.2.3.4}'::ltree[] ?<@ '1.2.5') is null;
select '{ltree.asd, tree.awdfg}'::ltree[] ?@ 'tree & aWdfg@'::ltxtquery;
select '{j.k.l.m, g.b.c.d.e}'::ltree[] ?~ 'A*@|g.b.c.d.e';

create table ltreetest (t ltree);
\copy ltreetest from 'data/ltree.data'

select * from ltreetest where t <  '12.3' order by t asc;
select * from ltreetest where t <= '12.3' order by t asc;
select * from ltreetest where t =  '12.3' order by t asc;
select * from ltreetest where t >= '12.3' order by t asc;
select * from ltreetest where t >  '12.3' order by t asc;
select * from ltreetest where t @> '1.1.1' order by t asc;
select * from ltreetest where t <@ '1.1.1' order by t asc;
select * from ltreetest where t ~ '1.1.1.*' order by t asc;
select * from ltreetest where t ~ '*.1' order by t asc;
select * from ltreetest where t ~ '23.*.1' order by t asc;
select * from ltreetest where t ~ '23.*{1}.1' order by t asc;
select * from ltreetest where t @ '23 & 1' order by t asc;

create unique index tstidx on ltreetest (t);
set enable_seqscan=off;

select * from ltreetest where t <  '12.3' order by t asc;
select * from ltreetest where t <= '12.3' order by t asc;
select * from ltreetest where t =  '12.3' order by t asc;
select * from ltreetest where t >= '12.3' order by t asc;
select * from ltreetest where t >  '12.3' order by t asc;

drop index tstidx;
create index tstidx on ltreetest using gist (t);
set enable_seqscan=off;

select * from ltreetest where t <  '12.3' order by t asc;
select * from ltreetest where t <= '12.3' order by t asc;
select * from ltreetest where t =  '12.3' order by t asc;
select * from ltreetest where t >= '12.3' order by t asc;
select * from ltreetest where t >  '12.3' order by t asc;
select * from ltreetest where t @> '1.1.1' order by t asc;
select * from ltreetest where t <@ '1.1.1' order by t asc;
select * from ltreetest where t ~ '1.1.1.*' order by t asc;
select * from ltreetest where t ~ '*.1' order by t asc;
select * from ltreetest where t ~ '23.*.1' order by t asc;
select * from ltreetest where t ~ '23.*{1}.1' order by t asc;
select * from ltreetest where t @ '23 & 1' order by t asc;

create table _ltreetest (t ltree[]);
\copy _ltreetest from 'data/_ltree.data'

select count(*) from _ltreetest where t @> '1.1.1' ;
select count(*) from _ltreetest where t <@ '1.1.1' ;
select count(*) from _ltreetest where t ~ '1.1.1.*' ;
select count(*) from _ltreetest where t ~ '*.1' ;
select count(*) from _ltreetest where t ~ '23.*.1' ;
select count(*) from _ltreetest where t ~ '23.*{1}.1' ;
select count(*) from _ltreetest where t @ '23 & 1' ;

create index _tstidx on _ltreetest using gist (t);
set enable_seqscan=off;

select count(*) from _ltreetest where t @> '1.1.1' ;
select count(*) from _ltreetest where t <@ '1.1.1' ;
select count(*) from _ltreetest where t ~ '1.1.1.*' ;
select count(*) from _ltreetest where t ~ '*.1' ;
select count(*) from _ltreetest where t ~ '23.*.1' ;
select count(*) from _ltreetest where t ~ '23.*{1}.1' ;
select count(*) from _ltreetest where t @ '23 & 1' ;

