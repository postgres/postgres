
- - -- load the new functions
- - --
load '/home/dz/lib/postgres/string_output.so';

- - -- create function c_textin(opaque)
- - --   returns text
- - --   as '/home/dz/lib/postgres/string_output.so' 
- - --   language 'c';

create function c_charout(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_char2out(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_char4out(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_char8out(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_char16out(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_textout(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

create function c_varcharout(opaque)
  returns int4
  as '/home/dz/lib/postgres/string_output.so' 
  language 'c';

- - -- define a function which sets the new output routines for char types
- - --
- - --   select c_mode();
- - --
create function c_mode()
  returns text
  as 'update pg_type set typoutput=''c_charout''    where typname=''char''\;
      update pg_type set typoutput=''c_char2out''   where typname=''char2''\;
      update pg_type set typoutput=''c_char4out''   where typname=''char4''\;
      update pg_type set typoutput=''c_char8out''   where typname=''char8''\;
      update pg_type set typoutput=''c_char16out''  where typname=''char16''\;
      update pg_type set typoutput=''c_textout''    where typname=''text''\;
      update pg_type set typoutput=''c_textout''    where typname=''bytea''\;
      update pg_type set typoutput=''c_textout''    where typname=''unknown''\;
      update pg_type set typoutput=''c_textout''    where typname=''SET''\;
      update pg_type set typoutput=''c_varcharout'' where typname=''varchar''\;
      update pg_type set typoutput=''c_varcharout'' where typname=''bpchar''\;
      select ''c_mode''::text'
  language 'sql';

- - -- define a function which restores the original routines for char types
- - --
- - --   select pg_mode();
- - --
create function pg_mode()
  returns text
  as 'update pg_type set typoutput=''charout''    where typname=''char''\;
      update pg_type set typoutput=''char2out''   where typname=''char2''\;
      update pg_type set typoutput=''char4out''   where typname=''char4''\;
      update pg_type set typoutput=''char8out''   where typname=''char8''\;
      update pg_type set typoutput=''char16out''  where typname=''char16''\;
      update pg_type set typoutput=''textout''    where typname=''text''\;
      update pg_type set typoutput=''textout''    where typname=''bytea''\;
      update pg_type set typoutput=''textout''    where typname=''unknown''\;
      update pg_type set typoutput=''textout''    where typname=''SET''\;
      update pg_type set typoutput=''varcharout'' where typname=''varchar''\;
      update pg_type set typoutput=''varcharout'' where typname=''bpchar''\;
      select ''pg_mode''::text'
  language 'sql';

- - -- or do the changes manually
- - --
- - -- update pg_type set typoutput='charout'    where typname='char';
- - -- update pg_type set typoutput='char2out'   where typname='char2';
- - -- update pg_type set typoutput='char4out'   where typname='char4';
- - -- update pg_type set typoutput='char8out'   where typname='char8';
- - -- update pg_type set typoutput='char16out'  where typname='char16';
- - -- update pg_type set typoutput='textout'    where typname='text';
- - -- update pg_type set typoutput='textout'    where typname='bytea';
- - -- update pg_type set typoutput='textout'    where typname='unknown';
- - -- update pg_type set typoutput='textout'    where typname='SET';
- - -- update pg_type set typoutput='varcharout' where typname='varchar';
- - -- update pg_type set typoutput='varcharout' where typname='bpchar';
- - --
- - -- update pg_type set typoutput='c_charout'    where typname='char';
- - -- update pg_type set typoutput='c_char2out'   where typname='char2';
- - -- update pg_type set typoutput='c_char4out'   where typname='char4';
- - -- update pg_type set typoutput='c_char8out'   where typname='char8';
- - -- update pg_type set typoutput='c_char16out'  where typname='char16';
- - -- update pg_type set typoutput='c_textout'    where typname='text';
- - -- update pg_type set typoutput='c_textout'    where typname='bytea';
- - -- update pg_type set typoutput='c_textout'    where typname='unknown';
- - -- update pg_type set typoutput='c_textout'    where typname='SET';
- - -- update pg_type set typoutput='c_varcharout' where typname='varchar';
- - -- update pg_type set typoutput='c_varcharout' where typname='bpchar';

