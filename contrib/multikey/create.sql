drop function create_mki_2 (text, text, text, text);
drop function create_mki_3 (text, text, text, text, text);
drop function create_mki_4 (text, text, text, text, text, text);

create function create_mki_2 (text, text, text, text)
returns int4 as '/home/postgres/My/Btree/MULTIKEY/multikey.so'
language 'c';

create function create_mki_3 (text, text, text, text, text)
returns int4 as '/home/postgres/My/Btree/MULTIKEY/multikey.so'
language 'c';

create function create_mki_4 (text, text, text, text, text, text)
returns int4 as '/home/postgres/My/Btree/MULTIKEY/multikey.so'
language 'c';
