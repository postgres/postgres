package org.postgresql.test.jdbc2.optional;

import org.postgresql.jdbc2.optional.ConnectionPool;
import org.postgresql.test.TestUtil;
import javax.sql.*;
import java.sql.*;
import java.io.*;

/**
 * Tests for the ConnectionPoolDataSource and PooledConnection
 * implementations.  They are tested together because the only client
 * interface to the PooledConnection is through the CPDS.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.6.4.4 $
 */
public class ConnectionPoolTest extends BaseDataSourceTest
{
	/**
	 * Constructor required by JUnit
	 */
	public ConnectionPoolTest(String name)
	{
		super(name);
	}

	/**
	 * Creates and configures a ConnectionPool
	 */
	protected void initializeDataSource()
	{
		if (bds == null)
		{
			bds = new ConnectionPool();
			bds.setServerName(TestUtil.getServer());
			bds.setPortNumber(TestUtil.getPort());
			bds.setDatabaseName(TestUtil.getDatabase());
			bds.setUser(TestUtil.getUser());
			bds.setPassword(TestUtil.getPassword());
		}
	}

	/**
	 * Though the normal client interface is to grab a Connection, in
	 * order to test the middleware/server interface, we need to deal
	 * with PooledConnections.	Some tests use each.
	 */
	protected PooledConnection getPooledConnection() throws SQLException
	{
		initializeDataSource();
		return ((ConnectionPool)bds).getPooledConnection();
	}

	/**
	 * Instead of just fetching a Connection from the ConnectionPool,
	 * get a PooledConnection, add a listener to close it when the
	 * Connection is closed, and then get the Connection.  Without
	 * the listener the PooledConnection (and thus the physical connection)
	 * would never by closed.  Probably not a disaster during testing, but
	 * you never know.
	 */
	protected Connection getDataSourceConnection() throws SQLException
	{
		initializeDataSource();
		final PooledConnection pc = getPooledConnection();
		// Since the pooled connection won't be reused in these basic tests, close it when the connection is closed
		pc.addConnectionEventListener(new ConnectionEventListener()
									  {
										  public void connectionClosed(ConnectionEvent event)
										  {
											  try
											  {
												  pc.close();
											  }
											  catch (SQLException e)
											  {
												  fail("Unable to close PooledConnection: " + e);
											  }
										  }

										  public void connectionErrorOccurred(ConnectionEvent event)
										  {}
									  }
									 );
		return pc.getConnection();
	}

	/**
	 * Makes sure that if you get a connection from a PooledConnection,
	 * close it, and then get another one, you're really using the same
	 * physical connection.  Depends on the implementation of toString
	 * for the connection handle.
	 */
	public void testPoolReuse()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			con = pc.getConnection();
			String name = con.toString();
			con.close();
			con = pc.getConnection();
			String name2 = con.toString();
			con.close();
			pc.close();
			assertTrue("Physical connection doesn't appear to be reused across PooledConnection wrappers", name.equals(name2));
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure that when you request a connection from the
	 * PooledConnection, and previous connection it might have given
	 * out is closed.  See JDBC 2.0 Optional Package spec section
	 * 6.2.3
	 */
	public void testPoolCloseOldWrapper()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			con = pc.getConnection();
			Connection con2 = pc.getConnection();
			try
			{
				con.createStatement();
				fail("Original connection wrapper should be closed when new connection wrapper is generated");
			}
			catch (SQLException e)
			{}
			con2.close();
			pc.close();
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure that if you get two connection wrappers from the same
	 * PooledConnection, they are different, even though the represent
	 * the same physical connection.  See JDBC 2.0 Optional Pacakge spec
	 * section 6.2.2
	 */
	public void testPoolNewWrapper()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			con = pc.getConnection();
			Connection con2 = pc.getConnection();
			con2.close();
			pc.close();
			assertTrue("Two calls to PooledConnection.getConnection should not return the same connection wrapper", con != con2);
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure that exactly one close event is fired for each time a
	 * connection handle is closed.  Also checks that events are not
	 * fired after a given handle has been closed once.
	 */
	public void testCloseEvent()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			CountClose cc = new CountClose();
			pc.addConnectionEventListener(cc);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 0);
			assertTrue(cc.getErrorCount() == 0);
			con.close();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			con.close();
			assertTrue(cc.getCount() == 2);
			assertTrue(cc.getErrorCount() == 0);
			// a double close shouldn't fire additional events
				con.close();
			assertTrue(cc.getCount() == 2);
			assertTrue(cc.getErrorCount() == 0);
			pc.close();
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure that close events are not fired after a listener has
	 * been removed.
	 */
	public void testNoCloseEvent()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			CountClose cc = new CountClose();
			pc.addConnectionEventListener(cc);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 0);
			assertTrue(cc.getErrorCount() == 0);
			con.close();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			pc.removeConnectionEventListener(cc);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			con.close();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure that a listener can be removed while dispatching
	 * events.	Sometimes this causes a ConcurrentModificationException
	 * or something.
	 */
	public void testInlineCloseEvent()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			RemoveClose rc1 = new RemoveClose();
			RemoveClose rc2 = new RemoveClose();
			RemoveClose rc3 = new RemoveClose();
			pc.addConnectionEventListener(rc1);
			pc.addConnectionEventListener(rc2);
			pc.addConnectionEventListener(rc3);
			con = pc.getConnection();
			con.close();
			con = pc.getConnection();
			con.close();
		}
		catch (Exception e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Tests that a close event is not generated when a connection
	 * handle is closed automatically due to a new connection handle
	 * being opened for the same PooledConnection.	See JDBC 2.0
	 * Optional Package spec section 6.3
	 */
	public void testAutomaticCloseEvent()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			CountClose cc = new CountClose();
			pc.addConnectionEventListener(cc);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 0);
			assertTrue(cc.getErrorCount() == 0);
			con.close();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			con = pc.getConnection();
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			// Open a 2nd connection, causing the first to be closed.  No even should be generated.
			Connection con2 = pc.getConnection();
			assertTrue("Connection handle was not closed when new handle was opened", con.isClosed());
			assertTrue(cc.getCount() == 1);
			assertTrue(cc.getErrorCount() == 0);
			con2.close();
			assertTrue(cc.getCount() == 2);
			assertTrue(cc.getErrorCount() == 0);
			pc.close();
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

	/**
	 * Makes sure the isClosed method on a connection wrapper does what
	 * you'd expect.  Checks the usual case, as well as automatic
	 * closure when a new handle is opened on the same physical connection.
	 */
	public void testIsClosed()
	{
		try
		{
			PooledConnection pc = getPooledConnection();
			Connection con = pc.getConnection();
			assertTrue(!con.isClosed());
			con.close();
			assertTrue(con.isClosed());
			con = pc.getConnection();
			Connection con2 = pc.getConnection();
			assertTrue(con.isClosed());
			assertTrue(!con2.isClosed());
			con2.close();
			assertTrue(con.isClosed());
			pc.close();
		}
		catch (SQLException e)
		{
			fail(e.getMessage());
		}
	}

    /**
     * Ensures that a statement generated by a proxied connection returns the
     * proxied connection from getConnection() [not the physical connection].
     */
    public void testStatementConnection() {
        try {
            PooledConnection pc = getPooledConnection();
            Connection con = pc.getConnection();
            Statement s = con.createStatement();
            Connection conRetrieved = s.getConnection();

            assertTrue(con.getClass().equals(conRetrieved.getClass()));
            assertTrue(con.equals(conRetrieved));
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * Ensures that the Statement proxy generated by the Connection handle
     * throws the correct kind of exception.
     */
    public void testStatementProxy() {
            Statement s = null;
            try 
            {
                    PooledConnection pc = getPooledConnection();
                    Connection con = pc.getConnection();
                    s = con.createStatement();
            } 
            catch (SQLException e) 
            {
                    fail(e.getMessage());
            }
            try 
            {
                    s.executeQuery("SELECT * FROM THIS_TABLE_SHOULD_NOT_EXIST");
                    fail("An SQL exception was not thrown that should have been");
            } 
            catch (SQLException e) 
            {
                    ; // This is the expected and correct path
            }
            catch (Exception e) 
            {
                    fail("bad exception; was expecting SQLException, not" +
                         e.getClass().getName());
            }
    }

    /**
     * Ensures that a prepared statement generated by a proxied connection
     * returns the proxied connection from getConnection() [not the physical
     * connection].
     */
    public void testPreparedStatementConnection() {
        try {
            PooledConnection pc = getPooledConnection();
            Connection con = pc.getConnection();
            PreparedStatement s = con.prepareStatement("select 'x'");
            Connection conRetrieved = s.getConnection();

            assertTrue(con.getClass().equals(conRetrieved.getClass()));
            assertTrue(con.equals(conRetrieved));
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * Ensures that a callable statement generated by a proxied connection
     * returns the proxied connection from getConnection() [not the physical
     * connection].
     */
    public void testCallableStatementConnection() {
        try {
            PooledConnection pc = getPooledConnection();
            Connection con = pc.getConnection();
            CallableStatement s = con.prepareCall("select 'x'");
            Connection conRetrieved = s.getConnection();

            assertTrue(con.getClass().equals(conRetrieved.getClass()));
            assertTrue(con.equals(conRetrieved));
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * Ensure that a statement created from a pool can be used
     * like any other statement in regard to pg extensions.
     */
    public void testStatementsProxyPGStatement() {
	try {
	    PooledConnection pc = getPooledConnection();
	    con = pc.getConnection();
	    
	    Statement s = con.createStatement();
	    boolean b = ((org.postgresql.PGStatement)s).isUseServerPrepare();

	    PreparedStatement ps = con.prepareStatement("select 'x'");
	    b = ((org.postgresql.PGStatement)ps).isUseServerPrepare();

            CallableStatement cs = con.prepareCall("select 'x'");
	    b = ((org.postgresql.PGStatement)cs).isUseServerPrepare();

	} catch (SQLException e) {
	    fail(e.getMessage());
	}
    }

    /**
	 * Helper class to remove a listener during event dispatching.
	 */
	private class RemoveClose implements ConnectionEventListener
	{
		public void connectionClosed(ConnectionEvent event)
		{
			((PooledConnection)event.getSource()).removeConnectionEventListener(this);
		}

		public void connectionErrorOccurred(ConnectionEvent event)
		{
			((PooledConnection)event.getSource()).removeConnectionEventListener(this);
		}
	}

	/**
	 * Helper class that implements the event listener interface, and
	 * counts the number of events it sees.
	 */
	private class CountClose implements ConnectionEventListener
	{
		private int count = 0, errorCount = 0;
		public void connectionClosed(ConnectionEvent event)
		{
			count++;
		}

		public void connectionErrorOccurred(ConnectionEvent event)
		{
			errorCount++;
		}

		public int getCount()
		{
			return count;
		}

		public int getErrorCount()
		{
			return errorCount;
		}

		public void clear()
		{
			count = errorCount = 0;
		}
	}

	public void testSerializable() throws IOException, ClassNotFoundException
	{
		ConnectionPool pool = new ConnectionPool();
		pool.setDefaultAutoCommit(false);
		pool.setServerName("db.myhost.com");
		pool.setDatabaseName("mydb");
		pool.setUser("user");
		pool.setPassword("pass");
		pool.setPortNumber(1111);

		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		ObjectOutputStream oos = new ObjectOutputStream(baos);
		oos.writeObject(pool);

		ByteArrayInputStream bais = new ByteArrayInputStream(baos.toByteArray());
		ObjectInputStream ois = new ObjectInputStream(bais);
		ConnectionPool pool2 = (ConnectionPool)ois.readObject();

		assertEquals(pool.isDefaultAutoCommit(),pool2.isDefaultAutoCommit());
		assertEquals(pool.getServerName(),pool2.getServerName());
		assertEquals(pool.getDatabaseName(),pool2.getDatabaseName());
		assertEquals(pool.getUser(),pool2.getUser());
		assertEquals(pool.getPassword(),pool2.getPassword());
		assertEquals(pool.getPortNumber(),pool2.getPortNumber());
	}

}
