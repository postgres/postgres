--
-- COMMENTS
--

SELECT 'trailing' AS first; -- trailing single line
SELECT /* embedded single line */ 'embedded' AS second;
SELECT /* both embedded and trailing single line */ 'both' AS third; -- trailing single line

SELECT 'before multi-line' AS fourth;
/* This is an example of SQL which should not execute:
 * select 'multi-line';
 */
SELECT 'after multi-line' AS fifth;

/* and this is the end of the file */

