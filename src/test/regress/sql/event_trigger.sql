-- should fail, return type mismatch
create event trigger regress_event_trigger
   on ddl_command_start
   execute procedure pg_backend_pid();

-- OK
create function test_event_trigger() returns event_trigger as $$
BEGIN
    RAISE NOTICE 'test_event_trigger: % %', tg_event, tg_tag;
END
$$ language plpgsql;

-- should fail, no elephant_bootstrap entry point
create event trigger regress_event_trigger on elephant_bootstrap
   execute procedure test_event_trigger();

-- OK
create event trigger regress_event_trigger on ddl_command_start
   execute procedure test_event_trigger();

-- OK
create event trigger regress_event_trigger_end on ddl_command_end
   execute procedure test_event_trigger();

-- should fail, food is not a valid filter variable
create event trigger regress_event_trigger2 on ddl_command_start
   when food in ('sandwhich')
   execute procedure test_event_trigger();

-- should fail, sandwhich is not a valid command tag
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('sandwhich')
   execute procedure test_event_trigger();

-- should fail, create skunkcabbage is not a valid comand tag
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table', 'create skunkcabbage')
   execute procedure test_event_trigger();

-- should fail, can't have event triggers on event triggers
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('DROP EVENT TRIGGER')
   execute procedure test_event_trigger();

-- should fail, can't have same filter variable twice
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table') and tag in ('CREATE FUNCTION')
   execute procedure test_event_trigger();

-- OK
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table', 'CREATE FUNCTION')
   execute procedure test_event_trigger();

-- OK
comment on event trigger regress_event_trigger is 'test comment';

-- should fail, event triggers are not schema objects
comment on event trigger wrong.regress_event_trigger is 'test comment';

-- drop as non-superuser should fail
create role regression_bob;
set role regression_bob;
create event trigger regress_event_trigger_noperms on ddl_command_start
   execute procedure test_event_trigger();
reset role;

-- all OK
alter event trigger regress_event_trigger enable replica;
alter event trigger regress_event_trigger enable always;
alter event trigger regress_event_trigger enable;
alter event trigger regress_event_trigger disable;

-- regress_event_trigger2 and regress_event_trigger_end should fire, but not
-- regress_event_trigger
create table event_trigger_fire1 (a int);

-- regress_event_trigger_end should fire here
drop table event_trigger_fire1;

-- alter owner to non-superuser should fail
alter event trigger regress_event_trigger owner to regression_bob;

-- alter owner to superuser should work
alter role regression_bob superuser;
alter event trigger regress_event_trigger owner to regression_bob;

-- should fail, name collision
alter event trigger regress_event_trigger rename to regress_event_trigger2;

-- OK
alter event trigger regress_event_trigger rename to regress_event_trigger3;

-- should fail, doesn't exist any more
drop event trigger regress_event_trigger;

-- should fail, regression_bob owns regress_event_trigger2/3
drop role regression_bob;

-- cleanup before next test
-- these are all OK; the second one should emit a NOTICE
drop event trigger if exists regress_event_trigger2;
drop event trigger if exists regress_event_trigger2;
drop event trigger regress_event_trigger3;
drop event trigger regress_event_trigger_end;

-- test support for dropped objects
CREATE SCHEMA schema_one authorization regression_bob;
CREATE SCHEMA schema_two authorization regression_bob;
CREATE SCHEMA audit_tbls authorization regression_bob;
SET SESSION AUTHORIZATION regression_bob;

CREATE TABLE schema_one.table_one(a int);
CREATE TABLE schema_one."table two"(a int);
CREATE TABLE schema_one.table_three(a int);
CREATE TABLE audit_tbls.schema_one_table_two(the_value text);

CREATE TABLE schema_two.table_two(a int);
CREATE TABLE schema_two.table_three(a int, b text);
CREATE TABLE audit_tbls.schema_two_table_three(the_value text);

CREATE OR REPLACE FUNCTION schema_two.add(int, int) RETURNS int LANGUAGE plpgsql
  CALLED ON NULL INPUT
  AS $$ BEGIN RETURN coalesce($1,0) + coalesce($2,0); END; $$;
CREATE AGGREGATE schema_two.newton
  (BASETYPE = int, SFUNC = schema_two.add, STYPE = int);

RESET SESSION AUTHORIZATION;

CREATE TABLE undroppable_objs (
	object_type text,
	object_identity text
);
INSERT INTO undroppable_objs VALUES
('table', 'schema_one.table_three'),
('table', 'audit_tbls.schema_two_table_three');

CREATE TABLE dropped_objects (
	type text,
	schema text,
	object text
);

-- This tests errors raised within event triggers; the one in audit_tbls
-- uses 2nd-level recursive invocation via test_evtrig_dropped_objects().
CREATE OR REPLACE FUNCTION undroppable() RETURNS event_trigger
LANGUAGE plpgsql AS $$
DECLARE
	obj record;
BEGIN
	PERFORM 1 FROM pg_tables WHERE tablename = 'undroppable_objs';
	IF NOT FOUND THEN
		RAISE NOTICE 'table undroppable_objs not found, skipping';
		RETURN;
	END IF;
	FOR obj IN
		SELECT * FROM pg_event_trigger_dropped_objects() JOIN
			undroppable_objs USING (object_type, object_identity)
	LOOP
		RAISE EXCEPTION 'object % of type % cannot be dropped',
			obj.object_identity, obj.object_type;
	END LOOP;
END;
$$;

CREATE EVENT TRIGGER undroppable ON sql_drop
	EXECUTE PROCEDURE undroppable();

CREATE OR REPLACE FUNCTION test_evtrig_dropped_objects() RETURNS event_trigger
LANGUAGE plpgsql AS $$
DECLARE
    obj record;
BEGIN
    FOR obj IN SELECT * FROM pg_event_trigger_dropped_objects()
    LOOP
        IF obj.object_type = 'table' THEN
                EXECUTE format('DROP TABLE IF EXISTS audit_tbls.%I',
					format('%s_%s', obj.schema_name, obj.object_name));
        END IF;

	INSERT INTO dropped_objects
		(type, schema, object) VALUES
		(obj.object_type, obj.schema_name, obj.object_identity);
    END LOOP;
END
$$;

CREATE EVENT TRIGGER regress_event_trigger_drop_objects ON sql_drop
	WHEN TAG IN ('drop table', 'drop function', 'drop view',
		'drop owned', 'drop schema', 'alter table')
	EXECUTE PROCEDURE test_evtrig_dropped_objects();

ALTER TABLE schema_one.table_one DROP COLUMN a;
DROP SCHEMA schema_one, schema_two CASCADE;
DELETE FROM undroppable_objs WHERE object_identity = 'audit_tbls.schema_two_table_three';
DROP SCHEMA schema_one, schema_two CASCADE;
DELETE FROM undroppable_objs WHERE object_identity = 'schema_one.table_three';
DROP SCHEMA schema_one, schema_two CASCADE;

SELECT * FROM dropped_objects WHERE schema IS NULL OR schema <> 'pg_toast';

DROP OWNED BY regression_bob;
SELECT * FROM dropped_objects WHERE type = 'schema';

DROP ROLE regression_bob;

DROP EVENT TRIGGER regress_event_trigger_drop_objects;
DROP EVENT TRIGGER undroppable;
