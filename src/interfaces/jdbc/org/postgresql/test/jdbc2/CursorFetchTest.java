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
			ResultSet rs = stmt.executeQuery();
 
			int count = 0;
			while (rs.next()) {
				assertEquals("query value error with fetch size " + testSizes[i], count, rs.getInt(1));
				++count;
			}
 
			assertEquals("total query size error with fetch size " + testSizes[i], 100, count);
		}
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
