--
-- crypt() and gen_salt(): crypt-des
--

select crypt('', 'NB');

select crypt('foox', 'NB');

create table ctest (data text, res text, salt text);
insert into ctest values ('password', '', '');

update ctest set salt = gen_salt('des');
update ctest set res = crypt(data, salt);
select res = crypt(data, res) as "worked" from ctest;

drop table ctest;

