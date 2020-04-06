-- xid and xid8

-- values in range, in octal, decimal, hex
select '010'::xid,
       '42'::xid,
       '0xffffffff'::xid,
       '-1'::xid,
	   '010'::xid8,
	   '42'::xid8,
	   '0xffffffffffffffff'::xid8,
	   '-1'::xid8;

-- garbage values are not yet rejected (perhaps they should be)
select ''::xid;
select 'asdf'::xid;
select ''::xid8;
select 'asdf'::xid8;

-- equality
select '1'::xid = '1'::xid;
select '1'::xid != '1'::xid;
select '1'::xid8 = '1'::xid8;
select '1'::xid8 != '1'::xid8;

-- conversion
select '1'::xid = '1'::xid8::xid;
select '1'::xid != '1'::xid8::xid;

-- we don't want relational operators for xid, due to use of modular arithmetic
select '1'::xid < '2'::xid;
select '1'::xid <= '2'::xid;
select '1'::xid > '2'::xid;
select '1'::xid >= '2'::xid;

-- we want them for xid8 though
select '1'::xid8 < '2'::xid8, '2'::xid8 < '2'::xid8, '2'::xid8 < '1'::xid8;
select '1'::xid8 <= '2'::xid8, '2'::xid8 <= '2'::xid8, '2'::xid8 <= '1'::xid8;
select '1'::xid8 > '2'::xid8, '2'::xid8 > '2'::xid8, '2'::xid8 > '1'::xid8;
select '1'::xid8 >= '2'::xid8, '2'::xid8 >= '2'::xid8, '2'::xid8 >= '1'::xid8;

-- we also have a 3way compare for btrees
select xid8cmp('1', '2'), xid8cmp('2', '2'), xid8cmp('2', '1');

-- xid8 has btree and hash opclasses
create table xid8_t1 (x xid8);
create index on xid8_t1 using btree(x);
create index on xid8_t1 using hash(x);
drop table xid8_t1;
