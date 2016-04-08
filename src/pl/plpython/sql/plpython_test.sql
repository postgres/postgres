-- first some tests of basic functionality
CREATE EXTENSION plpython2u;

-- really stupid function just to get the module loaded
CREATE FUNCTION stupid() RETURNS text AS 'return "zarkon"' LANGUAGE plpythonu;

select stupid();

-- check 2/3 versioning
CREATE FUNCTION stupidn() RETURNS text AS 'return "zarkon"' LANGUAGE plpython2u;

select stupidn();

-- test multiple arguments and odd characters in function name
CREATE FUNCTION "Argument test #1"(u users, a1 text, a2 text) RETURNS text
	AS
'keys = list(u.keys())
keys.sort()
out = []
for key in keys:
    out.append("%s: %s" % (key, u[key]))
words = a1 + " " + a2 + " => {" + ", ".join(out) + "}"
return words'
	LANGUAGE plpythonu;

select "Argument test #1"(users, fname, lname) from users where lname = 'doe' order by 1;


-- check module contents
CREATE FUNCTION module_contents() RETURNS text AS
$$
contents = list(filter(lambda x: not x.startswith("__"), dir(plpy)))
contents.sort()
return ", ".join(contents)
$$ LANGUAGE plpythonu;

select module_contents();

CREATE FUNCTION elog_test_basic() RETURNS void
AS $$
plpy.debug('debug')
plpy.log('log')
plpy.info('info')
plpy.info(37)
plpy.info()
plpy.info('info', 37, [1, 2, 3])
plpy.notice('notice')
plpy.warning('warning')
plpy.error('error')
$$ LANGUAGE plpythonu;

SELECT elog_test_basic();

CREATE FUNCTION elog_test() RETURNS void
AS $$
plpy.debug('debug', detail = 'some detail')
plpy.log('log', detail = 'some detail')
plpy.info('info', detail = 'some detail')
plpy.info()
plpy.info('the question', detail = 42);
plpy.info('This is message text.',
                    detail = 'This is detail text',
                    hint = 'This is hint text.',
                    sqlstate = 'XX000',
                    schema = 'any info about schema',
                    table = 'any info about table',
                    column = 'any info about column',
                    datatype = 'any info about datatype',
                    constraint = 'any info about constraint')
plpy.notice('notice', detail = 'some detail')
plpy.warning('warning', detail = 'some detail')
plpy.error('stop on error', detail = 'some detail', hint = 'some hint')
$$ LANGUAGE plpythonu;

SELECT elog_test();

do $$ plpy.info('other types', detail = (10,20)) $$ LANGUAGE plpythonu;

do $$
import time;
from datetime import date
plpy.info('other types', detail = date(2016,2,26))
$$ LANGUAGE plpythonu;

do $$
basket = ['apple', 'orange', 'apple', 'pear', 'orange', 'banana']
plpy.info('other types', detail = basket)
$$ LANGUAGE plpythonu;

-- should fail
do $$ plpy.info('wrong sqlstate', sqlstate='54444A') $$ LANGUAGE plpythonu;
do $$ plpy.info('unsupported argument', blabla='fooboo') $$ LANGUAGE plpythonu;
do $$ plpy.info('first message', message='second message') $$ LANGUAGE plpythonu;
do $$ plpy.info('first message', 'second message', message='third message') $$ LANGUAGE plpythonu;

-- raise exception in python, handle exception in plgsql
CREATE OR REPLACE FUNCTION raise_exception(_message text, _detail text DEFAULT NULL, _hint text DEFAULT NULL,
						_sqlstate text DEFAULT NULL,
						_schema text DEFAULT NULL, _table text DEFAULT NULL, _column text DEFAULT NULL,
						_datatype text DEFAULT NULL, _constraint text DEFAULT NULL)
RETURNS void AS $$
kwargs = { "message":_message, "detail":_detail, "hint":_hint,
			"sqlstate":_sqlstate, "schema":_schema, "table":_table,
			"column":_column, "datatype":_datatype, "constraint":_constraint }
# ignore None values
plpy.error(**dict((k, v) for k, v in iter(kwargs.items()) if v))
$$ LANGUAGE plpythonu;

SELECT raise_exception('hello', 'world');
SELECT raise_exception('message text', 'detail text', _sqlstate => 'YY333');
SELECT raise_exception(_message => 'message text',
						_detail => 'detail text',
						_hint => 'hint text',
						_sqlstate => 'XX555',
						_schema => 'schema text',
						_table => 'table text',
						_column => 'column text',
						_datatype => 'datatype text',
						_constraint => 'constraint text');

SELECT raise_exception(_message => 'message text',
						_hint => 'hint text',
						_schema => 'schema text',
						_column => 'column text',
						_constraint => 'constraint text');

DO $$
DECLARE
  __message text;
  __detail text;
  __hint text;
  __sqlstate text;
  __schema_name text;
  __table_name text;
  __column_name text;
  __datatype text;
  __constraint text;
BEGIN
  BEGIN
    PERFORM raise_exception(_message => 'message text',
                            _detail => 'detail text',
                            _hint => 'hint text',
                            _sqlstate => 'XX555',
                            _schema => 'schema text',
                            _table => 'table text',
                            _column => 'column text',
                            _datatype => 'datatype text',
                            _constraint => 'constraint text');
  EXCEPTION WHEN SQLSTATE 'XX555' THEN
    GET STACKED DIAGNOSTICS __message = MESSAGE_TEXT,
                            __detail = PG_EXCEPTION_DETAIL,
                            __hint = PG_EXCEPTION_HINT,
                            __sqlstate = RETURNED_SQLSTATE,
                            __schema_name = SCHEMA_NAME,
                            __table_name = TABLE_NAME,
                            __column_name = COLUMN_NAME,
                            __datatype = PG_DATATYPE_NAME,
                            __constraint = CONSTRAINT_NAME;
    RAISE NOTICE 'handled exception'
       USING DETAIL = format('message:(%s), detail:(%s), hint: (%s), sqlstate: (%s), '
                             'schema:(%s), table:(%s), column:(%s), datatype:(%s), constraint:(%s)',
                             __message, __detail, __hint, __sqlstate, __schema_name,
                             __table_name, __column_name, __datatype, __constraint);
  END;
END;
$$;

-- the displayed context is different between Python2 and Python3,
-- but that's not important for this test
\set SHOW_CONTEXT never

do $$
try:
	plpy.execute("select raise_exception(_message => 'my message', _sqlstate => 'XX987', _hint => 'some hint', _table=> 'users_tab', _datatype => 'user_type')")
except Exception, e:
	plpy.info(e.spidata)
	raise e
$$ LANGUAGE plpythonu;

do $$
try:
  plpy.error(message  = 'my message', sqlstate = 'XX987', hint = 'some hint', table = 'users_tab', datatype = 'user_type')
except Exception, e:
  plpy.info('sqlstate: %s, hint: %s, tablename: %s, datatype: %s' % (e.sqlstate, e.hint, e.table_name, e.datatype_name))
  raise e
$$ LANGUAGE plpythonu;
