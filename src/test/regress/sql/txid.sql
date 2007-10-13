-- txid_snapshot data type and related functions

-- i/o
select '12:13:'::txid_snapshot;
select '12:18:14,16'::txid_snapshot;

-- errors
select '31:12:'::txid_snapshot;
select '0:1:'::txid_snapshot;
select '12:13:0'::txid_snapshot;
select '12:16:14,13'::txid_snapshot;
select '12:16:14,14'::txid_snapshot;

create temp table snapshot_test (
	nr	integer,
	snap	txid_snapshot
);

insert into snapshot_test values (1, '12:13:');
insert into snapshot_test values (2, '12:20:13,15,18');
insert into snapshot_test values (3, '100001:100009:100005,100007,100008');
insert into snapshot_test values (4, '100:150:101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131');
select snap from snapshot_test order by nr;

select  txid_snapshot_xmin(snap),
	txid_snapshot_xmax(snap),
	txid_snapshot_xip(snap)
from snapshot_test order by nr;

select id, txid_visible_in_snapshot(id, snap)
from snapshot_test, generate_series(11, 21) id
where nr = 2;

-- test bsearch
select id, txid_visible_in_snapshot(id, snap)
from snapshot_test, generate_series(90, 160) id
where nr = 4;

-- test current values also
select txid_current() >= txid_snapshot_xmin(txid_current_snapshot());

-- we can't assume current is always less than xmax, however

select txid_visible_in_snapshot(txid_current(), txid_current_snapshot());

-- test 64bitness

select txid_snapshot '1000100010001000:1000100010001100:1000100010001012,1000100010001013';
select txid_visible_in_snapshot('1000100010001012', '1000100010001000:1000100010001100:1000100010001012,1000100010001013');
select txid_visible_in_snapshot('1000100010001015', '1000100010001000:1000100010001100:1000100010001012,1000100010001013');

-- test 64bit overflow
SELECT txid_snapshot '1:9223372036854775807:3';
SELECT txid_snapshot '1:9223372036854775808:3';
