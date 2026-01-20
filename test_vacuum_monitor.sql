-- Monitor VACUUM progress in real-time
-- Run this in a separate psql session while VACUUM is running on test_vacuum_art

-- Enable expanded display for better readability
\x

-- Continuously monitor vacuum progress
-- Press Ctrl+C to stop
\watch 2

SELECT 
    pid,
    datname,
    relid::regclass AS table_name,
    phase,
    heap_blks_total,
    heap_blks_scanned,
    heap_blks_vacuumed,
    index_vacuum_count,
    
    -- Postgres 17 specific metrics
    max_dead_tuple_bytes,
    dead_tuple_bytes,
    
    -- Postgres 16 and earlier metrics  
    max_dead_tuples,
    num_dead_tuples,
    
    indexes_total,
    indexes_processed,
    
    -- Calculate progress percentage
    CASE 
        WHEN heap_blks_total > 0 
        THEN round(100.0 * heap_blks_scanned / heap_blks_total, 2)
        ELSE 0 
    END AS scan_progress_pct
    
FROM pg_stat_progress_vacuum
WHERE relid::regclass::text = 'test_vacuum_art';
