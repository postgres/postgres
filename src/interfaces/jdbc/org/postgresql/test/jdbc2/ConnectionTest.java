package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * TestCase to test the internal functionality of org.postgresql.jdbc2.Connection
 * and it's superclass.
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: ConnectionTest.java,v 1.10.6.1 2004/02/24 13:11:44 jurka Exp $
 */

public class ConnectionTest extends TestCase
{

	/*
	 * Constructor
	 */
	public ConnectionTest(String name)
	{
		super(name);
	}

	// Set up the fixture for this testcase: the tables for this test.
	protected void setUp() throws Exception
	{
		Connection con = TestUtil.openDB();

		TestUtil.createTable(con, "test_a", "imagename name,image oid,id int4");
		TestUtil.createTable(con, "test_c", "source text,cost money,imageid int4");

		TestUtil.closeDB(con);
	}

	// Tear down the fixture for this test case.
	protected void tearDown() throws Exception
	{
		Connection con = TestUtil.openDB();

		TestUtil.dropTable(con, "test_a");
		TestUtil.dropTable(con, "test_c");

		TestUtil.closeDB(con);
	}

	/*
	 * Tests the two forms of createStatement()
	 */
	public void testCreateStatement()
	{
		try
		{
			java.sql.Connection conn = TestUtil.openDB();

			// A standard Statement
			java.sql.Statement stat = conn.createStatement();
			assertNotNull(stat);
			stat.close();

			// Ask for Updateable ResultSets
			stat = conn.createStatement(java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE, java.sql.ResultSet.CONCUR_UPDATABLE);
			assertNotNull(stat);
			stat.close();

		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/*
	 * Tests the two forms of prepareStatement()
	 */
	public void testPrepareStatement()
	{
		try
		{
			java.sql.Connection conn = TestUtil.openDB();

			String sql = "select source,cost,imageid from test_c";

			// A standard Statement
			java.sql.PreparedStatement stat = conn.prepareStatement(sql);
			assertNotNull(stat);
			stat.close();

			// Ask for Updateable ResultSets
			stat = conn.prepareStatement(sql, java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE, java.sql.ResultSet.CONCUR_UPDATABLE);
			assertNotNull(stat);
			stat.close();

		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/*
	 * Put the test for createPrepareCall here
	 */
	public void testPrepareCall()
	{}

	/*
	 * Test nativeSQL
	 */
	public void testNativeSQL()
	{
		// For now do nothing as it returns itself
	}

	/*
	 * Test autoCommit (both get & set)
	 */
	public void testTransactions()
	{
		try
		{
			java.sql.Connection con = TestUtil.openDB();
			java.sql.Statement st;
			java.sql.ResultSet rs;

			// Turn it off
			con.setAutoCommit(false);
			assertTrue(!con.getAutoCommit());

			// Turn it back on
			con.setAutoCommit(true);
			assertTrue(con.getAutoCommit());

			// Now test commit
			st = con.createStatement();
			st.executeUpdate("insert into test_a (imagename,image,id) values ('comttest',1234,5678)");

			con.setAutoCommit(false);

			// Now update image to 9876 and commit
			st.executeUpdate("update test_a set image=9876 where id=5678");
			con.commit();
			rs = st.executeQuery("select image from test_a where id=5678");
			assertTrue(rs.next());
			assertEquals(9876, rs.getInt(1));
			rs.close();

			// Now try to change it but rollback
			st.executeUpdate("update test_a set image=1111 where id=5678");
			con.rollback();
			rs = st.executeQuery("select image from test_a where id=5678");
			assertTrue(rs.next());
			assertEquals(9876, rs.getInt(1)); // Should not change!
			rs.close();

			TestUtil.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/*
	 * Simple test to see if isClosed works.
	 */
	public void testIsClosed()
	{
		try
		{
			Connection con = TestUtil.openDB();

			// Should not say closed
			assertTrue(!con.isClosed());

			TestUtil.closeDB(con);

			// Should now say closed
			assertTrue(con.isClosed());

		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/*
	 * Test the warnings system
	 */
	public void testWarnings()
	{
		try
		{
			Connection con = TestUtil.openDB();

			String testStr = "This Is OuR TeSt message";

			// The connection must be ours!
			assertTrue(con instanceof org.postgresql.PGConnection);

			// Clear any existing warnings
			con.clearWarnings();

			// Set the test warning
			((org.postgresql.jdbc2.AbstractJdbc2Connection)con).addWarning(testStr);

			// Retrieve it
			SQLWarning warning = con.getWarnings();
			assertNotNull(warning);
			assertEquals(testStr, warning.getMessage());

			// Finally test clearWarnings() this time there must be something to delete
			con.clearWarnings();
			assertTrue(con.getWarnings() == null);

			TestUtil.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/*
	 * Transaction Isolation Levels
	 */
	public void testTransactionIsolation()
	{
		try
		{
			Connection con = TestUtil.openDB();

			// PostgreSQL defaults to READ COMMITTED
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			// Begin a transaction
			con.setAutoCommit(false);

			// The isolation level should not have changed
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());


			// Note the behavior on when a transaction starts is different
			// under 7.3 than previous versions.  In 7.3 a transaction 
			// starts with the first sql command, whereas previously 
			// you were always in a transaction in autocommit=false
            // so we issue a select to ensure we are in a transaction
            Statement stmt = con.createStatement();
            stmt.executeQuery("select 1");

			// Now change the default for future transactions
			con.setTransactionIsolation(Connection.TRANSACTION_SERIALIZABLE);

			// Since the call to setTransactionIsolation() above was made
		   	// inside the transaction, the isolation level of the current
		   	// transaction did not change. It affects only future ones.
		   	// This behaviour is recommended by the JDBC spec.
		   	assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

		   	// Begin a new transaction
		   	con.commit();
            stmt.executeQuery("select 1");

			// Now we should see the new isolation level
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());

			// Repeat the steps above with the transition back to
			// READ COMMITTED.
			con.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED);
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());
			con.commit();
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			// Now run some tests with autocommit enabled.
			con.setAutoCommit(true);

			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			con.setTransactionIsolation(Connection.TRANSACTION_SERIALIZABLE);
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());

			con.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED);
			assertEquals(Connection.TRANSACTION_READ_COMMITTED, con.getTransactionIsolation());

			// Test if a change of isolation level before beginning the
			// transaction affects the isolation level inside the transaction.
			con.setTransactionIsolation(Connection.TRANSACTION_SERIALIZABLE);
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());
			con.setAutoCommit(false);
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());
			con.setAutoCommit(true);
			assertEquals(Connection.TRANSACTION_SERIALIZABLE,
						 con.getTransactionIsolation());
			con.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED);
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());
			con.setAutoCommit(false);
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			TestUtil.closeDB(con);
		}
		catch ( SQLException ex )
		{
			fail( ex.getMessage() );
		}
	}

	/*
	 * JDBC2 Type mappings
	 */
	public void testTypeMaps()
	{
		try
		{
			Connection con = TestUtil.openDB();

			// preserve the current map
			java.util.Map oldmap = con.getTypeMap();

			// now change it for an empty one
			java.util.Map newmap = new java.util.HashMap();
			con.setTypeMap(newmap);
			assertEquals(newmap, con.getTypeMap());

			// restore the old one
			con.setTypeMap(oldmap);
			assertEquals(oldmap, con.getTypeMap());

			TestUtil.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/**
	 * Closing a Connection more than once is not an error.
	 */
	public void testDoubleClose() throws SQLException
	{
		Connection con = TestUtil.openDB();
		con.close();
		con.close();
	}
}
