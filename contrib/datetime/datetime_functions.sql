func=$1
cat <<% > datetime_functions.sql
drop function time_difference(time,time);
drop function currentdate();
drop function currenttime();
drop function hours(time);
drop function minutes(time);
drop function seconds(time);
drop function day(date);
drop function month(date);
drop function year(date);
drop function asminutes(time);
drop function asseconds(time);
drop operator - (time,time);

create function time_difference(time,time)
  returns time
  as '$func' 
  language 'c';

create function currentdate()
  returns date
  as '$func' 
  language 'c';

create function currenttime()
  returns time
  as '$func' 
  language 'c';

create function hours(time)
  returns int4
  as '$func' 
  language 'c';

create function minutes(time)
  returns int4
  as '$func' 
  language 'c';

create function seconds(time)
  returns int4
  as '$func' 
  language 'c';

create function day(date)
  returns int4
  as '$func'
  language 'c';

create function month(date)
  returns int4
  as '$func'
  language 'c';

create function year(date)
  returns int4
  as '$func'
  language 'c';

create function asminutes(time)
  returns int4
  as '$func' 
  language 'c';

create function asseconds(time)
  returns int4
  as '$func' 
  language 'c';

create operator - (
  leftarg=time, 
  rightarg=time, 
  procedure=time_difference);
%
