-- Adjust this setting to control where the objects get created.
SET search_path = public;

SET autocommit TO 'on';

CREATE TRIGGER "MyTableName_Trig" 
AFTER INSERT OR DELETE OR UPDATE ON "MyTableName"
FOR EACH ROW EXECUTE PROCEDURE "recordchange" ();

