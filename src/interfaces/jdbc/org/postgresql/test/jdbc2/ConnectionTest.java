package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * TestCase to test the internal functionality of org.postgresql.jdbc2.Connection
 * and it's superclass.
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: ConnectionTest.java,v 1.6 2001/10/25 05:59:59 momjian Exp $
 */

public class ConnectionTest extends TestCase
{

	/**
	 * Constructor
	 */
	public ConnectionTest(String name)
	{
		super(name);
	}

	// Set up the fixture for this testcase: the tables for this test.
	protected void setUp() throws Exception
	{
		Connection con = JDBC2Tests.openDB();

		JDBC2Tests.createTable(con, "test_a", "imagename name,image oid,id int4");
		JDBC2Tests.createTable(con, "test_c", "source text,cost money,imageid int4");

		JDBC2Tests.closeDB(con);
	}

	// Tear down the fixture for this test case.
	protected void tearDown() throws Exception
	{
		Connection con = JDBC2Tests.openDB();

		JDBC2Tests.dropTable(con, "test_a");
		JDBC2Tests.dropTable(con, "test_c");

		JDBC2Tests.closeDB(con);
	}

	/**
	 * Tests the two forms of createStatement()
	 */
	public void testCreateStatement()
	{
		try
		{
			java.sql.Connection conn = JDBC2Tests.openDB();

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

	/**
	 * Tests the two forms of prepareStatement()
	 */
	public void testPrepareStatement()
	{
		try
		{
			java.sql.Connection conn = JDBC2Tests.openDB();

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

	/**
	 * Put the test for createPrepareCall here
	 */
	public void testPrepareCall()
	{}

	/**
	 * Test nativeSQL
	 */
	public void testNativeSQL()
	{
		// For now do nothing as it returns itself
	}

	/**
	 * Test autoCommit (both get & set)
	 */
	public void testTransactions()
	{
		try
		{
			java.sql.Connection con = JDBC2Tests.openDB();
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

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/**
	 * Simple test to see if isClosed works.
	 */
	public void testIsClosed()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			// Should not say closed
			assertTrue(!con.isClosed());

			JDBC2Tests.closeDB(con);

			// Should now say closed
			assertTrue(con.isClosed());

		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/**
	 * Test the warnings system
	 */
	public void testWarnings()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			String testStr = "This Is OuR TeSt message";

			// The connection must be ours!
			assertTrue(con instanceof org.postgresql.Connection);

			// Clear any existing warnings
			con.clearWarnings();

			// Set the test warning
			((org.postgresql.Connection)con).addWarning(testStr);

			// Retrieve it
			SQLWarning warning = con.getWarnings();
			assertNotNull(warning);
			assertEquals(testStr, warning.getMessage());

			// Finally test clearWarnings() this time there must be something to delete
			con.clearWarnings();
			assertTrue(con.getWarnings() == null);

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}

	/**
	 * Transaction Isolation Levels
	 */
	public void testTransactionIsolation()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			// PostgreSQL defaults to READ COMMITTED
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			// Begin a transaction
			con.setAutoCommit(false);

			// The isolation level should not have changed
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			// Now change the default for future transactions
			con.setTransactionIsolation(Connection.TRANSACTION_SERIALIZABLE);

			// Since the call to setTransactionIsolation() above was made
			// inside the transaction, the isolation level of the current
			// transaction did not change. It affects only future transactions.
			// This behaviour is recommended by the JDBC spec.
			assertEquals(Connection.TRANSACTION_READ_COMMITTED,
						 con.getTransactionIsolation());

			// Begin a new transaction
			con.commit();

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

			JDBC2Tests.closeDB(con);
		}
		catch ( SQLException ex )
		{
			fail( ex.getMessage() );
		}
	}

	/**
	 * JDBC2 Type mappings
	 */
	public void testTypeMaps()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			// preserve the current map
			java.util.Map oldmap = con.getTypeMap();

			// now change it for an empty one
			java.util.Map newmap = new java.util.HashMap();
			con.setTypeMap(newmap);
			assertEquals(newmap, con.getTypeMap());

			// restore the old one
			con.setTypeMap(oldmap);
			assertEquals(oldmap, con.getTypeMap());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			assertTrue(ex.getMessage(), false);
		}
	}
}
