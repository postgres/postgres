

DROP FUNCTION txid_current();
DROP FUNCTION txid_current_snapshot();
DROP FUNCTION txid_snapshot_xmin(txid_snapshot);
DROP FUNCTION txid_snapshot_xmax(txid_snapshot);
DROP FUNCTION txid_snapshot_xip(txid_snapshot);
DROP FUNCTION txid_visible_in_snapshot(bigint, txid_snapshot);
DROP FUNCTION txid_not_visible_in_snapshot(bigint, txid_snapshot);

DROP TYPE txid_snapshot cascade;
-- need cascade to drop those:
-- DROP FUNCTION txid_snapshot_in(cstring);
-- DROP FUNCTION txid_snapshot_out(txid_snapshot);

