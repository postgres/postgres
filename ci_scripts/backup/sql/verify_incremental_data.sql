-- ===============================================
-- 1. Verify TDE Tables Exist
-- ===============================================
SELECT table_name
FROM information_schema.tables
WHERE table_schema = 'public'
AND table_name IN ('tde_table', 'tde_child', 'audit_log', 'part_table', 'part1')
ORDER BY table_name;

-- ===============================================
-- 2. Verify Columns of Tables
-- ===============================================
SELECT column_name, data_type, table_name
FROM information_schema.columns
WHERE table_name IN ('tde_table', 'tde_child', 'audit_log', 'part_table', 'part1')
ORDER BY table_name, ordinal_position;

-- ===============================================
-- 3. Verify Constraints Exist
-- ===============================================
SELECT conname, conrelid::regclass, contype
FROM pg_constraint
WHERE connamespace = 'public'::regnamespace
AND conrelid::regclass::text IN ('tde_table', 'tde_child')
ORDER BY conrelid;

-- ===============================================
-- 4. Verify Index Exists
-- ===============================================
SELECT indexname, tablename
FROM pg_indexes
WHERE schemaname = 'public' AND tablename = 'tde_table';

-- ===============================================
-- 5. Verify Functions Exist
-- ===============================================
SELECT proname, prorettype::regtype
FROM pg_proc
JOIN pg_namespace ON pg_proc.pronamespace = pg_namespace.oid
WHERE nspname = 'public'
AND proname = 'get_tde_data';

-- ===============================================
-- 6. Verify Function Output
-- ===============================================
SELECT * FROM get_tde_data();

-- ===============================================
-- 7. Verify Partitioning
-- ===============================================
SELECT inhrelid::regclass AS partition_name, inhparent::regclass AS parent_table
FROM pg_inherits
WHERE inhparent::regclass::text = 'part_table'
ORDER BY inhparent;

-- ===============================================
-- 8. Verify Triggers Exist
-- ===============================================
SELECT tgname, relname
FROM pg_trigger
JOIN pg_class ON pg_trigger.tgrelid = pg_class.oid
WHERE NOT tgisinternal AND relname = 'tde_table';

-- ===============================================
-- 9. Verify Data Integrity
-- ===============================================
-- Check data counts
SELECT 'tde_table' AS table_name, COUNT(*) FROM tde_table
UNION ALL
SELECT 'tde_child', COUNT(*) FROM tde_child
UNION ALL
SELECT 'audit_log', COUNT(*) FROM audit_log
UNION ALL
SELECT 'part_table', COUNT(*) FROM part_table;

-- Ensure tde_child references valid parent_id
SELECT tde_child.id, tde_child.parent_id
FROM tde_child
LEFT JOIN tde_table ON tde_child.parent_id = tde_table.id
WHERE tde_table.id IS NULL;

-- ===============================================
-- 10. Verify tables are encrypted
-- ===============================================
-- Verify all tables exist and are encrypted
SELECT tablename, pg_tde_is_encrypted(tablename::TEXT) AS is_encrypted
FROM pg_tables
WHERE schemaname = 'public'
AND tablename IN ('tde_table', 'tde_child', 'part1','part_table')
ORDER BY tablename;

