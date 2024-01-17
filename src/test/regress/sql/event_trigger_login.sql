-- Login event triggers
CREATE TABLE user_logins(id serial, who text);
GRANT SELECT ON user_logins TO public;
CREATE FUNCTION on_login_proc() RETURNS event_trigger AS $$
BEGIN
  INSERT INTO user_logins (who) VALUES (SESSION_USER);
  RAISE NOTICE 'You are welcome!';
END;
$$ LANGUAGE plpgsql;
CREATE EVENT TRIGGER on_login_trigger ON login EXECUTE PROCEDURE on_login_proc();
ALTER EVENT TRIGGER on_login_trigger ENABLE ALWAYS;
\c
SELECT COUNT(*) FROM user_logins;
\c
SELECT COUNT(*) FROM user_logins;

-- Check dathasloginevt in system catalog
SELECT dathasloginevt FROM pg_database WHERE datname= :'DBNAME';

-- Cleanup
DROP TABLE user_logins;
DROP EVENT TRIGGER on_login_trigger;
DROP FUNCTION on_login_proc();
\c
