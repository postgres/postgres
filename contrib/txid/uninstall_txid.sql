SET search_path = public;

DROP FUNCTION txid_current();
DROP FUNCTION txid_current_snapshot();
DROP FUNCTION txid_snapshot_xmin(txid_snapshot);
DROP FUNCTION txid_snapshot_xmax(txid_snapshot);
DROP FUNCTION txid_snapshot_xip(txid_snapshot);
DROP FUNCTION txid_visible_in_snapshot(bigint, txid_snapshot);

DROP TYPE txid_snapshot CASCADE;
-- need cascade to drop the I/O functions
