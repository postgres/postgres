psql postgres

BEGIN;
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;
UPDATE ycsb SET value = 'modified-xx-2' WHERE key = '2';
