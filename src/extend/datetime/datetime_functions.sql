
-- SQL code to load and define 'datetime' functions

-- load the new functions

load '/home/dz/lib/postgres/datetime_functions.so';

-- define the new functions in postgres

create function time_difference(time,time)
  returns time
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function currentDate()
  returns date
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function currentTime()
  returns time
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function hours(time)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function minutes(time)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function seconds(time)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function day(date)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so'
  language 'c';

create function month(date)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so'
  language 'c';

create function year(date)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so'
  language 'c';

create function asMinutes(time)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create function asSeconds(time)
  returns int4
  as '/home/dz/lib/postgres/datetime_functions.so' 
  language 'c';

create operator - (
  leftarg=time, 
  rightarg=time, 
  procedure=time_difference);

