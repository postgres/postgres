package org.postgresql.test.jdbc2;
 
import java.sql.*;
 
import junit.framework.TestCase;
 
import org.postgresql.test.TestUtil;
 
/*
 *  Tests for using non-zero setFetchSize().
 */
public class CursorFetchTest extends TestCase
{
	private Connection con;
 
	public CursorFetchTest(String name)
	{
		super(name);
	}
 
	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		TestUtil.createTable(con, "test_fetch", "value integer");
		con.setAutoCommit(false);
	}
 
	protected void tearDown() throws Exception
	{
		con.rollback();
		con.setAutoCommit(true);
		TestUtil.dropTable(con, "test_fetch");
		TestUtil.closeDB(con);
	}
 
	protected void createRows(int count) throws Exception
	{
		PreparedStatement stmt = con.prepareStatement("insert into test_fetch(value) values(?)");
		for (int i = 0; i < count; ++i) {
			stmt.setInt(1,i);
			stmt.executeUpdate();
		}
	}

	// Test various fetchsizes.
	public void testBasicFetch() throws Exception
	{
		createRows(100);
 
		PreparedStatement stmt = con.prepareStatement("select * from test_fetch order by value");
		int[] testSizes = { 0, 1, 49, 50, 51, 99, 100, 101 };
		for (int i = 0; i < testSizes.length; ++i) {
			stmt.setFetchSize(testSizes[i]);
			assertEquals(testSizes[i], stmt.getFetchSize());

			ResultSet rs = stmt.executeQuery();
			assertEquals(testSizes[i], rs.getFetchSize());
 
			int count = 0;
			while (rs.next()) {
				assertEquals("query value error with fetch size " + testSizes[i], count, rs.getInt(1));
				++count;
			}
 
			assertEquals("total query size error with fetch size " + testSizes[i], 100, count);
		}
	}

	//
	// Tests for ResultSet.setFetchSize().
	//

	// test one:
	//   set fetchsize = 0
	//   run query (all rows should be fetched)
	//   set fetchsize = 50 (should have no effect)
	//   process results
	public void testResultSetFetchSizeOne() throws Exception
	{
		createRows(100);

		PreparedStatement stmt = con.prepareStatement("select * from test_fetch order by value");
		stmt.setFetchSize(0);
		ResultSet rs = stmt.executeQuery();
		rs.setFetchSize(50); // Should have no effect.

		int count = 0;		
		while (rs.next()) {
			assertEquals(count, rs.getInt(1));
			++count;
		}

		assertEquals(100, count);
	}

	// test two:
	//   set fetchsize = 25
	//   run query (25 rows fetched)
	//   set fetchsize = 0
	//   process results:
	//     process 25 rows
	//     should do a FETCH ALL to get more data
	//     process 75 rows
	public void testResultSetFetchSizeTwo() throws Exception
	{
		createRows(100);

		PreparedStatement stmt = con.prepareStatement("select * from test_fetch order by value");
		stmt.setFetchSize(25);
		ResultSet rs = stmt.executeQuery();
		rs.setFetchSize(0);

		int count = 0;
		while (rs.next()) {
			assertEquals(count, rs.getInt(1));
			++count;
		}

		assertEquals(100, count);
	}

	// test three:
	//   set fetchsize = 25
	//   run query (25 rows fetched)
	//   set fetchsize = 50
	//   process results:
	//     process 25 rows. should NOT hit end-of-results here.
	//     do a FETCH FORWARD 50
	//     process 50 rows
	//     do a FETCH FORWARD 50
	//     process 25 rows. end of results.
	public void testResultSetFetchSizeThree() throws Exception
	{
		createRows(100);

		PreparedStatement stmt = con.prepareStatement("select * from test_fetch order by value");
		stmt.setFetchSize(25);
		ResultSet rs = stmt.executeQuery();
		rs.setFetchSize(50);

		int count = 0;
		while (rs.next()) {
			assertEquals(count, rs.getInt(1));
			++count;
		}

		assertEquals(100, count);
	}

	// test four:
	//   set fetchsize = 50
	//   run query (50 rows fetched)
	//   set fetchsize = 25
	//   process results:
	//     process 50 rows.
	//     do a FETCH FORWARD 25
	//     process 25 rows
	//     do a FETCH FORWARD 25
	//     process 25 rows. end of results.
	public void testResultSetFetchSizeFour() throws Exception
	{
		createRows(100);

		PreparedStatement stmt = con.prepareStatement("select * from test_fetch order by value");
		stmt.setFetchSize(50);
		ResultSet rs = stmt.executeQuery();
		rs.setFetchSize(25);

		int count = 0;
		while (rs.next()) {
			assertEquals(count, rs.getInt(1));
			++count;
		}

		assertEquals(100, count);
	}

	// Test odd queries that should not be transformed into cursor-based fetches.
	public void TODO_FAILS_testInsert() throws Exception
	{
		// INSERT should not be transformed.
		PreparedStatement stmt = con.prepareStatement("insert into test_fetch(value) values(1)");
		stmt.setFetchSize(100); // Should be meaningless.
		stmt.executeUpdate();
	}

	public void TODO_FAILS_testMultistatement() throws Exception
	{
		// Queries with multiple statements should not be transformed.

		createRows(100); // 0 .. 99
		PreparedStatement stmt = con.prepareStatement("insert into test_fetch(value) values(100); select * from test_fetch order by value");
		stmt.setFetchSize(10);
		ResultSet rs = stmt.executeQuery();
		
		int count = 0;
		while (rs.next()) {
			assertEquals(count, rs.getInt(1));
			++count;
		}
 
		assertEquals(101, count);
	}	
}
