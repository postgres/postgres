--
-- crypt() and gen_salt(): extended des
--

select crypt('', '_J9..j2zz');

select crypt('foox', '_J9..j2zz');

create table ctest (data text, res text, salt text);
insert into ctest values ('password', '', '');

update ctest set salt = gen_salt('xdes', 1001);
update ctest set res = crypt(data, salt);
select res = crypt(data, res) as "worked" from ctest;

drop table ctest;

