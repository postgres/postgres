package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/* TODO tests that can be added to this test case
 * - SQLExceptions chained to a BatchUpdateException
 * - test PreparedStatement as thoroughly as Statement
 */

/*
 * Test case for Statement.batchExecute()
 */
public class BatchExecuteTest extends TestCase
{

	private Connection con;

	public BatchExecuteTest(String name)
	{
		super(name);
	}

	// Set up the fixture for this testcase: a connection to a database with
	// a table for this test.
	protected void setUp() throws Exception
	{
		con = JDBC2Tests.openDB();
		Statement stmt = con.createStatement();

		// Drop the test table if it already exists for some reason. It is
		// not an error if it doesn't exist.
		JDBC2Tests.createTable(con, "testbatch", "pk INTEGER, col1 INTEGER");

		stmt.executeUpdate("INSERT INTO testbatch VALUES (1, 0)");

		// Generally recommended with batch updates. By default we run all
		// tests in this test case with autoCommit disabled.
		con.setAutoCommit(false);
	}

	// Tear down the fixture for this test case.
	protected void tearDown() throws Exception
	{
		con.setAutoCommit(true);

		JDBC2Tests.dropTable(con, "testbatch");
		JDBC2Tests.closeDB(con);
	}

	public void testSupportsBatchUpdates() throws Exception
	{
		DatabaseMetaData dbmd = con.getMetaData();
		assertTrue(dbmd.supportsBatchUpdates());
	}

	private void assertCol1HasValue(int expected) throws Exception
	{
		Statement getCol1 = con.createStatement();

		ResultSet rs =
			getCol1.executeQuery("SELECT col1 FROM testbatch WHERE pk = 1");
		assertTrue(rs.next());

		int actual = rs.getInt("col1");

		assertEquals(expected, actual);

		assertEquals(false, rs.next());

		rs.close();
		getCol1.close();
	}

	public void testExecuteEmptyBatch() throws Exception
	{
		Statement stmt = con.createStatement();
		int[] updateCount = stmt.executeBatch();
		assertEquals(0, updateCount.length);

		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 1 WHERE pk = 1");
		stmt.clearBatch();
		updateCount = stmt.executeBatch();
		assertEquals(0, updateCount.length);
		stmt.close();
	}

	public void testClearBatch() throws Exception
	{
		Statement stmt = con.createStatement();

		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 1 WHERE pk = 1");
		assertCol1HasValue(0);
		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 2 WHERE pk = 1");
		assertCol1HasValue(0);
		stmt.clearBatch();
		assertCol1HasValue(0);
		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 4 WHERE pk = 1");
		assertCol1HasValue(0);
		stmt.executeBatch();
		assertCol1HasValue(4);
		con.commit();
		assertCol1HasValue(4);

		stmt.close();
	}

	public void testSelectThrowsException() throws Exception
	{
		Statement stmt = con.createStatement();

		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 1 WHERE pk = 1");
		stmt.addBatch("SELECT col1 FROM testbatch WHERE pk = 1");
		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 2 WHERE pk = 1");

		try
		{
			stmt.executeBatch();
			fail("Should raise a BatchUpdateException because of the SELECT");
		}
		catch (BatchUpdateException e)
		{
			int [] updateCounts = e.getUpdateCounts();
			assertEquals(1, updateCounts.length);
			assertEquals(1, updateCounts[0]);
		}
		catch (SQLException e)
		{
			fail( "Should throw a BatchUpdateException instead of " +
				  "a generic SQLException: " + e);
		}

		stmt.close();
	}

	public void testPreparedStatement() throws Exception
	{
		PreparedStatement pstmt = con.prepareStatement(
									  "UPDATE testbatch SET col1 = col1 + ? WHERE PK = ?" );

		// Note that the first parameter changes for every statement in the
		// batch, whereas the second parameter remains constant.
		pstmt.setInt(1, 1);
		pstmt.setInt(2, 1);
		pstmt.addBatch();
		assertCol1HasValue(0);

		pstmt.setInt(1, 2);
		pstmt.addBatch();
		assertCol1HasValue(0);

		pstmt.setInt(1, 4);
		pstmt.addBatch();
		assertCol1HasValue(0);

		pstmt.executeBatch();
		assertCol1HasValue(7);

		con.commit();
		assertCol1HasValue(7);

		con.rollback();
		assertCol1HasValue(7);

		pstmt.close();
	}

	public void testTransactionalBehaviour() throws Exception
	{
		Statement stmt = con.createStatement();

		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 1 WHERE pk = 1");
		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 2 WHERE pk = 1");
		stmt.executeBatch();
		con.rollback();
		assertCol1HasValue(0);

		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 4 WHERE pk = 1");
		stmt.addBatch("UPDATE testbatch SET col1 = col1 + 8 WHERE pk = 1");

		// The statement has been added to the batch, but it should not yet
		// have been executed.
		assertCol1HasValue(0);

		int[] updateCounts = stmt.executeBatch();
		assertEquals(2, updateCounts.length);
		assertEquals(1, updateCounts[0]);
		assertEquals(1, updateCounts[1]);

		assertCol1HasValue(12);
		con.commit();
		assertCol1HasValue(12);
		con.rollback();
		assertCol1HasValue(12);

		stmt.close();
	}
}
