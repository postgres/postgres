--
-- This creates the database for the stock example
-- $PostgreSQL: pgsql/src/interfaces/jdbc/example/corba/stock.sql,v 1.2 2003/11/29 22:41:21 pgsql Exp $
--
drop table stock;

create table stock (
	id int4,
	avail int4,
	ordered int4,
	valid bool,
	description text
);

create index stock_id on stock(id);

copy stock from stdin;
1	19	0	t	Dell Latitude XPi P133 Laptop
2	3	2	t	Iomega Zip Plus drive
3	2	0	f	Iomega Ext/Par drive
4	0	4	t	Iomega Ext USB drive
5	200	0	t	Blank Unbranded CDR media
6	20	30	t	Iomega Zip media 100Mb
\.

grant all on stock to public;
grant all on stock_id to public;
