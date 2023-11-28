CREATE EXTENSION test_slru;

SELECT test_slru_page_exists(12345);
SELECT test_slru_page_write(12345, 'Test SLRU');
SELECT test_slru_page_read(12345);
SELECT test_slru_page_exists(12345);

-- 48 extra pages
SELECT count(test_slru_page_write(a, 'Test SLRU'))
  FROM generate_series(12346, 12393, 1) as a;

-- Reading page in buffer for read and write
SELECT test_slru_page_read(12377, true);
-- Reading page in buffer for read-only
SELECT test_slru_page_readonly(12377);
-- Reading page not in buffer with read-only
SELECT test_slru_page_readonly(12346);

-- Write all the pages in buffers
SELECT test_slru_page_writeall();
-- Flush the last page written out.
SELECT test_slru_page_sync(12393);
SELECT test_slru_page_exists(12393);
-- Segment deletion
SELECT test_slru_page_delete(12393);
SELECT test_slru_page_exists(12393);
-- Page truncation
SELECT test_slru_page_exists(12377);
SELECT test_slru_page_truncate(12377);
SELECT test_slru_page_exists(12377);

-- Full deletion
SELECT test_slru_delete_all();
SELECT test_slru_page_exists(12345);
SELECT test_slru_page_exists(12377);
SELECT test_slru_page_exists(12393);

--
-- Test 64-bit pages
--
SELECT test_slru_page_exists(0x1234500000000);
SELECT test_slru_page_write(0x1234500000000, 'Test SLRU 64-bit');
SELECT test_slru_page_read(0x1234500000000);
SELECT test_slru_page_exists(0x1234500000000);

-- 48 extra pages
SELECT count(test_slru_page_write(a, 'Test SLRU 64-bit'))
  FROM generate_series(0x1234500000001, 0x1234500000030, 1) as a;

-- Reading page in buffer for read and write
SELECT test_slru_page_read(0x1234500000020, true);
-- Reading page in buffer for read-only
SELECT test_slru_page_readonly(0x1234500000020);
-- Reading page not in buffer with read-only
SELECT test_slru_page_readonly(0x1234500000001);

-- Write all the pages in buffers
SELECT test_slru_page_writeall();
-- Flush the last page written out.
SELECT test_slru_page_sync(0x1234500000030);
SELECT test_slru_page_exists(0x1234500000030);
-- Segment deletion
SELECT test_slru_page_delete(0x1234500000030);
SELECT test_slru_page_exists(0x1234500000030);
-- Page truncation
SELECT test_slru_page_exists(0x1234500000020);
SELECT test_slru_page_truncate(0x1234500000020);
SELECT test_slru_page_exists(0x1234500000020);

-- Full deletion
SELECT test_slru_delete_all();
SELECT test_slru_page_exists(0x1234500000000);
SELECT test_slru_page_exists(0x1234500000020);
SELECT test_slru_page_exists(0x1234500000030);

DROP EXTENSION test_slru;
