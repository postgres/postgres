create table quoteTBL (f text);

insert into quoteTBL values ('hello world');
insert into quoteTBL values ('hello '' world');
insert into quoteTBL values ('hello \' world');
insert into quoteTBL values ('hello \\ world');
insert into quoteTBL values ('hello \t world');
insert into quoteTBL values ('hello
world
with 
newlines
');
insert into quoteTBL values ('hello " world');
insert into quoteTBL values ('');
  -- bad escape sequence 
insert into quoteTBL values ('hello \y world');  
select * from quoteTBL;
drop table quoteTBL;
