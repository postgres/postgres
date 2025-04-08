CREATE EXTENSION autoinc;

create sequence aitest_seq increment 10 start 0 minvalue 0;

create table aitest (
	price_id	int4,
	price_val	int4,
	price_on	int4
);

create trigger aiserial
	before insert or update on aitest
	for each row
	execute procedure
	autoinc (price_on, aitest_seq);

insert into aitest values (1, 1, null);
insert into aitest values (2, 2, 0);
insert into aitest values (3, 3, 1);

select * from aitest;

update aitest set price_on = 11;

select * from aitest;

update aitest set price_on = 0;

select * from aitest;

update aitest set price_on = null;

select * from aitest;
