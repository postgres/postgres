vacuum verbose analyze stat_t (i1,i2);
--
--
select i2,sum(b) as b
into tmp
from stat_t
where i1 >= '144.206.0.0' and i1 <= '144.206.255.255'
group by i2;
insert into tmp
select '0.0.0.0' as i2,sum(b) as k
from tmp
where b < 10 * 1024;
delete from tmp
where b < 10 * 1024 and i2 <> '0.0.0.0';
select i2,b / 1024 as k from tmp
order by k desc;
select ipaddr_print(i2,'%A/%P'),b from tmp where i2 = '0.0.0.0';
drop table tmp;
explain  select i2,sum(b) as b
into tmp
from stat_t
where i1 >= '144.206.0.0' and i1 <= '144.206.255.255'
group by i2;
-- ********************************************************
-- * Now remove test table by 'drop table stat_t' command *
-- ********************************************************
