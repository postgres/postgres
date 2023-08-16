--
-- OAT checks for ALTER TABLE
--

-- This test script fails if debug_discard_caches is enabled, because cache
-- flushes cause extra calls of the OAT hook in recomputeNamespacePath,
-- resulting in more NOTICE messages than are in the expected output.
SET debug_discard_caches = 0;

LOAD 'test_oat_hooks';
SET test_oat_hooks.audit = true;

CREATE SCHEMA test_oat_schema;
CREATE TABLE test_oat_schema.test_oat_tab (c1 int, c2 text);
CREATE RULE test_oat_notify AS
  ON UPDATE TO test_oat_schema.test_oat_tab
  DO ALSO NOTIFY test_oat_tab;

CREATE FUNCTION test_oat_schema.test_trigger()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
  IF TG_OP = 'DELETE'
  THEN
    RETURN OLD;
  ELSE
    RETURN NEW;
  END IF;
END; $$;
CREATE TRIGGER test_oat_trigger BEFORE INSERT ON test_oat_schema.test_oat_tab
  FOR EACH STATEMENT EXECUTE FUNCTION test_oat_schema.test_trigger();

-- RLS
ALTER TABLE test_oat_schema.test_oat_tab ENABLE ROW LEVEL SECURITY;
ALTER TABLE test_oat_schema.test_oat_tab DISABLE ROW LEVEL SECURITY;
ALTER TABLE test_oat_schema.test_oat_tab FORCE ROW LEVEL SECURITY;
ALTER TABLE test_oat_schema.test_oat_tab NO FORCE ROW LEVEL SECURITY;

-- Rules
ALTER TABLE test_oat_schema.test_oat_tab DISABLE RULE test_oat_notify;
ALTER TABLE test_oat_schema.test_oat_tab ENABLE RULE test_oat_notify;

-- Triggers
ALTER TABLE test_oat_schema.test_oat_tab DISABLE TRIGGER test_oat_trigger;
ALTER TABLE test_oat_schema.test_oat_tab ENABLE TRIGGER test_oat_trigger;

DROP TABLE test_oat_schema.test_oat_tab;
