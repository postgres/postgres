--
-- show_all_settings()
--
SELECT * FROM show_all_settings();

--
-- normal_rand()
--
SELECT * FROM normal_rand(100, 250, 5, EXTRACT(SECONDS FROM CURRENT_TIME(0))::int);

--
-- crosstab()
--
create table ct(id serial, rowclass text, rowid text, attribute text, value text);

insert into ct(rowclass, rowid, attribute, value) values('group1','test1','att1','val1');
insert into ct(rowclass, rowid, attribute, value) values('group1','test1','att2','val2');
insert into ct(rowclass, rowid, attribute, value) values('group1','test1','att3','val3');
insert into ct(rowclass, rowid, attribute, value) values('group1','test1','att4','val4');
insert into ct(rowclass, rowid, attribute, value) values('group1','test2','att1','val5');
insert into ct(rowclass, rowid, attribute, value) values('group1','test2','att2','val6');
insert into ct(rowclass, rowid, attribute, value) values('group1','test2','att3','val7');
insert into ct(rowclass, rowid, attribute, value) values('group1','test2','att4','val8');
insert into ct(rowclass, rowid, attribute, value) values('group2','test3','att1','val1');
insert into ct(rowclass, rowid, attribute, value) values('group2','test3','att2','val2');
insert into ct(rowclass, rowid, attribute, value) values('group2','test3','att3','val3');
insert into ct(rowclass, rowid, attribute, value) values('group2','test4','att1','val4');
insert into ct(rowclass, rowid, attribute, value) values('group2','test4','att2','val5');
insert into ct(rowclass, rowid, attribute, value) values('group2','test4','att3','val6');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group1'' and (attribute = ''att2'' or attribute = ''att3'') order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group2'' and (attribute = ''att1'' or attribute = ''att2'') order by 1,2;');

select * from crosstab2('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');
select * from crosstab3('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');
select * from crosstab4('select rowid, attribute, value from ct where rowclass = ''group2'' order by 1,2;');

select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 2) as c(rowid text, att1 text, att2 text);
select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 3) as c(rowid text, att1 text, att2 text, att3 text);
select * from crosstab('select rowid, attribute, value from ct where rowclass = ''group1'' order by 1,2;', 4) as c(rowid text, att1 text, att2 text, att3 text, att4 text);
