--
-- crypt() and gen_salt(): bcrypt
--

select crypt('', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

select crypt('foox', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

create table ctest (data text, res text, salt text);
insert into ctest values ('password', '', '');

update ctest set salt = gen_salt('bf', 8);
update ctest set res = crypt(data, salt);
select res = crypt(data, res) as "worked" from ctest;

drop table ctest;

