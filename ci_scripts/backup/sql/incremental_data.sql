-- Creating encrypted tables
CREATE TABLE tde_table (
    id SERIAL PRIMARY KEY, 
    name TEXT
) USING tde_heap;

CREATE TABLE tde_child (
    id SERIAL PRIMARY KEY, 
    parent_id INT REFERENCES tde_table(id)
) USING tde_heap;

-- Inserting test data
INSERT INTO tde_table (name) VALUES ('Alice'), ('Bob');
INSERT INTO tde_child (parent_id) VALUES (1);

-- Creating index on encrypted table
CREATE INDEX idx_tde_name ON tde_table(name);
SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename;

-- Adding constraints
ALTER TABLE tde_table ADD CONSTRAINT unique_name UNIQUE(name);
ALTER TABLE tde_table ADD CONSTRAINT check_name_length CHECK (LENGTH(name) > 3);

SELECT conname, conrelid::regclass, contype FROM pg_constraint WHERE connamespace = 'public'::regnamespace ORDER BY conrelid;

-- Creating test function
CREATE FUNCTION get_tde_data() RETURNS TABLE(id INT, name TEXT) AS $$
    SELECT * FROM tde_table;
$$ LANGUAGE SQL;

-- Running function
SELECT * FROM get_tde_data();

-- Creating partitioned table
CREATE TABLE part_table (
    id INT, 
    data TEXT
) PARTITION BY RANGE (id);

-- Creating partitions using TDE
CREATE TABLE part1 PARTITION OF part_table 
FOR VALUES FROM (1) TO (100) USING tde_heap;

-- Insert test data
INSERT INTO part_table VALUES (10, 'Partitioned Data');

-- Querying data
SELECT inhrelid::regclass, inhparent::regclass FROM pg_inherits ORDER BY inhparent;

-- Creating audit log table
CREATE TABLE audit_log (
    id SERIAL PRIMARY KEY,
    table_name TEXT,
    operation TEXT
) USING tde_heap;

-- Creating trigger function
CREATE FUNCTION audit_tde_changes() RETURNS TRIGGER AS $$
BEGIN 
    INSERT INTO audit_log (table_name, operation) 
    VALUES (TG_TABLE_NAME, TG_OP);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Creating trigger
CREATE TRIGGER tde_audit 
AFTER INSERT OR UPDATE OR DELETE ON tde_table 
FOR EACH ROW EXECUTE FUNCTION audit_tde_changes();

SELECT tgname, relname FROM pg_trigger JOIN pg_class ON pg_trigger.tgrelid = pg_class.oid WHERE NOT tgisinternal ORDER BY relname;

-- Check WAL logs for plaintext leaks
-- SELECT * FROM pg_walfile_name_offset(pg_current_wal_lsn());
