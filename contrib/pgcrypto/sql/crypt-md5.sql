--
-- crypt() and gen_salt(): md5
--

select crypt('', '$1$Szzz0yzz');

select crypt('foox', '$1$Szzz0yzz');

create table ctest (data text, res text, salt text);
insert into ctest values ('password', '', '');

update ctest set salt = gen_salt('md5');
update ctest set res = crypt(data, salt);
select res = crypt(data, res) as "worked" from ctest;

drop table ctest;

