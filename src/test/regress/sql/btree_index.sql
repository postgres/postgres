--
-- BTREE_INDEX
-- test retrieval of min/max keys for each index
--

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno < 1;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno >= 9999;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno = 4500;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno < '1'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno >= '9999'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno = '4500'::name;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno < '1'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno >= '9999'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno = '4500'::text;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno < '1'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno >= '9999'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno = '4500'::float8;

