-- Test extended TOAST header structures (Phase 0)
CREATE EXTENSION test_toast_ext;

-- Test 1: Structure sizes
SELECT test_toast_structure_sizes();

-- Test 2: Flag validation
SELECT test_toast_flag_validation();

-- Test 3: Compression ID constants
SELECT test_toast_compression_ids();
