-- Tests with portals set up by a Bind message.
-- All these rely on the unnamed portal with \bind.

CREATE EXTENSION injection_points;

-- Make the injection points local to this session.
SELECT injection_points_set_local();

-- Portal is new, not fully set up yet.
SELECT injection_points_attach('bind-portal-created', 'error');
-- No transaction block.
SELECT 1 \bind \g
SELECT 2;
-- Transaction block, not aborted after failure.
BEGIN;
SELECT 3 \bind \g
ROLLBACK;
SELECT 4;
-- Transaction block already aborted when failure happens.
BEGIN;
SELECT 1/0;
ROLLBACK \bind \g
ROLLBACK;
SELECT 5;
-- Sub-transaction already aborted when failure happens.
BEGIN;
SAVEPOINT s;
SELECT 1/0;
ROLLBACK TO s \bind \g
ROLLBACK TO s;
SELECT 6;
COMMIT;
SELECT injection_points_detach('bind-portal-created');

-- Portal is defined, not fully set up yet.
SELECT injection_points_attach('bind-portal-defined', 'error');
-- No transaction block.
SELECT 1 \bind \g
SELECT 2;
-- Transaction block, not aborted under failure.
BEGIN;
SELECT 3 \bind \g
ROLLBACK;
SELECT 4;
-- Transaction block already aborted when failure happens.
BEGIN;
SELECT 1/0;
ROLLBACK \bind \g
ROLLBACK;
SELECT 5;
-- Sub-transaction already aborted when failure happens.
BEGIN;
SAVEPOINT s;
SELECT 1/0;
ROLLBACK TO s \bind \g
ROLLBACK TO s;
SELECT 6;
COMMIT;
SELECT injection_points_detach('bind-portal-defined');

DROP EXTENSION injection_points;
