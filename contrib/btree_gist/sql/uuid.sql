-- uuid check

CREATE TABLE uuidtmp (a uuid);

\copy uuidtmp from 'data/uuid.data'

SET enable_seqscan=on;

SELECT count(*) FROM uuidtmp WHERE a <  '55e65ca2-4136-4a4b-ba78-cd3fe4678203';

SELECT count(*) FROM uuidtmp WHERE a <= '55e65ca2-4136-4a4b-ba78-cd3fe4678203';

SELECT count(*) FROM uuidtmp WHERE a  = '55e65ca2-4136-4a4b-ba78-cd3fe4678203';

SELECT count(*) FROM uuidtmp WHERE a >= '55e65ca2-4136-4a4b-ba78-cd3fe4678203';

SELECT count(*) FROM uuidtmp WHERE a >  '55e65ca2-4136-4a4b-ba78-cd3fe4678203';

SET client_min_messages = DEBUG1;
CREATE INDEX uuididx ON uuidtmp USING gist ( a );
CREATE INDEX uuididx_b ON uuidtmp USING gist ( a ) WITH (buffering=on);
DROP INDEX uuididx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM uuidtmp WHERE a <  '55e65ca2-4136-4a4b-ba78-cd3fe4678203'::uuid;

SELECT count(*) FROM uuidtmp WHERE a <= '55e65ca2-4136-4a4b-ba78-cd3fe4678203'::uuid;

SELECT count(*) FROM uuidtmp WHERE a  = '55e65ca2-4136-4a4b-ba78-cd3fe4678203'::uuid;

SELECT count(*) FROM uuidtmp WHERE a >= '55e65ca2-4136-4a4b-ba78-cd3fe4678203'::uuid;

SELECT count(*) FROM uuidtmp WHERE a >  '55e65ca2-4136-4a4b-ba78-cd3fe4678203'::uuid;
