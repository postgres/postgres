package org.postgresql.jdbc2.optional;

import javax.sql.*;
import java.sql.*;
import java.util.*;
import java.lang.reflect.*;
import org.postgresql.PGConnection;

/**
 * PostgreSQL implementation of the PooledConnection interface.  This shouldn't
 * be used directly, as the pooling client should just interact with the
 * ConnectionPool instead.
 * @see ConnectionPool
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @author Csaba Nagy (ncsaba@yahoo.com)
 * @version $Revision: 1.7.4.3 $
 */
public class PooledConnectionImpl implements PooledConnection
{
	private List listeners = new LinkedList();
	private Connection con;
	private ConnectionHandler last;
	private boolean autoCommit;

	/**
	 * Creates a new PooledConnection representing the specified physical
	 * connection.
	 */
	protected PooledConnectionImpl(Connection con, boolean autoCommit)
	{
		this.con = con;
		this.autoCommit = autoCommit;
	}

	/**
	 * Adds a listener for close or fatal error events on the connection
	 * handed out to a client.
	 */
	public void addConnectionEventListener(ConnectionEventListener connectionEventListener)
	{
		listeners.add(connectionEventListener);
	}

	/**
	 * Removes a listener for close or fatal error events on the connection
	 * handed out to a client.
	 */
	public void removeConnectionEventListener(ConnectionEventListener connectionEventListener)
	{
		listeners.remove(connectionEventListener);
	}

	/**
	 * Closes the physical database connection represented by this
	 * PooledConnection.  If any client has a connection based on
	 * this PooledConnection, it is forcibly closed as well.
	 */
	public void close() throws SQLException
	{
		if (last != null)
		{
			last.close();
			if (!con.getAutoCommit())
			{
				try
				{
					con.rollback();
				}
				catch (SQLException e)
				{}
			}
		}
		try
		{
			con.close();
		}
		finally
		{
			con = null;
		}
	}

	/**
	 * Gets a handle for a client to use.  This is a wrapper around the
	 * physical connection, so the client can call close and it will just
	 * return the connection to the pool without really closing the
	 * pgysical connection.
	 *
	 * <p>According to the JDBC 2.0 Optional Package spec (6.2.3), only one
	 * client may have an active handle to the connection at a time, so if
	 * there is a previous handle active when this is called, the previous
	 * one is forcibly closed and its work rolled back.</p>
	 */
	public Connection getConnection() throws SQLException
	{
		if (con == null)
		{
			// Before throwing the exception, let's notify the registered listeners about the error
			final SQLException sqlException = new SQLException("This PooledConnection has already been closed!");
			fireConnectionFatalError(sqlException);
			throw sqlException;
		}
		// If any error occures while opening a new connection, the listeners
		// have to be notified. This gives a chance to connection pools to
		// elliminate bad pooled connections.
		try
		{
			// Only one connection can be open at a time from this PooledConnection.  See JDBC 2.0 Optional Package spec section 6.2.3
			if (last != null)
			{
				last.close();
				if (!con.getAutoCommit())
				{
					try
					{
						con.rollback();
					}
					catch (SQLException e)
					{}
				}
				con.clearWarnings();
			}
			con.setAutoCommit(autoCommit);
		}
		catch (SQLException sqlException)
		{
			fireConnectionFatalError(sqlException);
			throw (SQLException)sqlException.fillInStackTrace();
		}
		ConnectionHandler handler = new ConnectionHandler(con);
		last = handler;
		Connection con = (Connection)Proxy.newProxyInstance(getClass().getClassLoader(), new Class[]{Connection.class, PGConnection.class}, handler);
		last.setProxy(con);
		return con;
	}

	/**
	 * Used to fire a connection closed event to all listeners.
	 */
	void fireConnectionClosed()
	{
		ConnectionEvent evt = null;
		// Copy the listener list so the listener can remove itself during this method call
		ConnectionEventListener[] local = (ConnectionEventListener[]) listeners.toArray(new ConnectionEventListener[listeners.size()]);
		for (int i = 0; i < local.length; i++)
		{
			ConnectionEventListener listener = local[i];
			if (evt == null)
			{
				evt = new ConnectionEvent(this);
			}
			listener.connectionClosed(evt);
		}
	}

	/**
	 * Used to fire a connection error event to all listeners.
	 */
	void fireConnectionFatalError(SQLException e)
	{
		ConnectionEvent evt = null;
		// Copy the listener list so the listener can remove itself during this method call
		ConnectionEventListener[] local = (ConnectionEventListener[])listeners.toArray(new ConnectionEventListener[listeners.size()]);
		for (int i = 0; i < local.length; i++)
		{
			ConnectionEventListener listener = local[i];
			if (evt == null)
			{
				evt = new ConnectionEvent(this, e);
			}
			listener.connectionErrorOccurred(evt);
		}
	}

	/**
	 * Instead of declaring a class implementing Connection, which would have
	 * to be updated for every JDK rev, use a dynamic proxy to handle all
	 * calls through the Connection interface.	This is the part that
	 * requires JDK 1.3 or higher, though JDK 1.2 could be supported with a
	 * 3rd-party proxy package.
	 */
	private class ConnectionHandler implements InvocationHandler
	{
		private Connection con;
        private Connection proxy; // the Connection the client is currently using, which is a proxy
		private boolean automatic = false;

		public ConnectionHandler(Connection con)
		{
			this.con = con;
		}

		public Object invoke(Object proxy, Method method, Object[] args)
		throws Throwable
		{
			// From Object
			if (method.getDeclaringClass().getName().equals("java.lang.Object"))
			{
				if (method.getName().equals("toString"))
				{
					return "Pooled connection wrapping physical connection " + con;
				}
				if (method.getName().equals("hashCode"))
				{
					return new Integer(con.hashCode());
				}
				if (method.getName().equals("equals"))
				{
					if (args[0] == null)
					{
						return Boolean.FALSE;
					}
					try
					{
						return Proxy.isProxyClass(args[0].getClass()) && ((ConnectionHandler) Proxy.getInvocationHandler(args[0])).con == con ? Boolean.TRUE : Boolean.FALSE;
					}
					catch (ClassCastException e)
					{
						return Boolean.FALSE;
					}
				}
                                try
                                {
                                    return method.invoke(con, args);
                                }
                                catch (InvocationTargetException e)
                                {
                                    throw e.getTargetException();
                                }
			}
			// All the rest is from the Connection or PGConnection interface
			if (method.getName().equals("isClosed"))
			{
				return con == null ? Boolean.TRUE : Boolean.FALSE;
			}
			if (con == null && !method.getName().equals("close"))
			{
				throw new SQLException(automatic ? "Connection has been closed automatically because a new connection was opened for the same PooledConnection or the PooledConnection has been closed" : "Connection has been closed");
			}
			if (method.getName().equals("close"))
			{
				// we are already closed and a double close
				// is not an error.
				if (con == null)
					return null;

				SQLException ex = null;
				if (!con.getAutoCommit())
				{
					try
					{
						con.rollback();
					}
					catch (SQLException e)
					{
						ex = e;
					}
				}
				con.clearWarnings();
				con = null;
                proxy = null;
				last = null;
				fireConnectionClosed();
				if (ex != null)
				{
					throw ex;
				}
				return null;
			}
            else if(method.getName().equals("createStatement"))
            {
                Statement st = (Statement)method.invoke(con, args);
                return Proxy.newProxyInstance(getClass().getClassLoader(), new Class[]{Statement.class, org.postgresql.PGStatement.class}, new StatementHandler(this, st));
            }
            else if(method.getName().equals("prepareCall"))
            {
                Statement st = (Statement)method.invoke(con, args);
                return Proxy.newProxyInstance(getClass().getClassLoader(), new Class[]{CallableStatement.class, org.postgresql.PGStatement.class}, new StatementHandler(this, st));
            }
            else if(method.getName().equals("prepareStatement"))
            {
                Statement st = (Statement)method.invoke(con, args);
                return Proxy.newProxyInstance(getClass().getClassLoader(), new Class[]{PreparedStatement.class, org.postgresql.PGStatement.class}, new StatementHandler(this, st));
            }
			else
			{
				return method.invoke(con, args);
			}
		}

        Connection getProxy() {
            return proxy;
        }

        void setProxy(Connection proxy) {
            this.proxy = proxy;
        }

		public void close()
		{
			if (con != null)
			{
				automatic = true;
			}
			con = null;
            proxy = null;
			// No close event fired here: see JDBC 2.0 Optional Package spec section 6.3
		}

        public boolean isClosed() {
            return con == null;
        }
	}

    /**
     * Instead of declaring classes implementing Statement, PreparedStatement,
     * and CallableStatement, which would have to be updated for every JDK rev,
     * use a dynamic proxy to handle all calls through the Statement
     * interfaces.	This is the part that requires JDK 1.3 or higher, though
     * JDK 1.2 could be supported with a 3rd-party proxy package.
     *
     * The StatementHandler is required in order to return the proper
     * Connection proxy for the getConnection method.
     */
    private static class StatementHandler implements InvocationHandler {
        private ConnectionHandler con;
        private Statement st;

        public StatementHandler(ConnectionHandler con, Statement st) {
            this.con = con;
            this.st = st;
        }
        public Object invoke(Object proxy, Method method, Object[] args)
        throws Throwable
        {
            // From Object
            if (method.getDeclaringClass().getName().equals("java.lang.Object"))
            {
                if (method.getName().equals("toString"))
                {
                    return "Pooled statement wrapping physical statement " + st;
                }
                if (method.getName().equals("hashCode"))
                {
                    return new Integer(st.hashCode());
                }
                if (method.getName().equals("equals"))
                {
                    if (args[0] == null)
                    {
                        return Boolean.FALSE;
                    }
                    try
                    {
                        return Proxy.isProxyClass(args[0].getClass()) && ((StatementHandler) Proxy.getInvocationHandler(args[0])).st == st ? Boolean.TRUE : Boolean.FALSE;
                    }
                    catch (ClassCastException e)
                    {
                        return Boolean.FALSE;
                    }
                }
                return method.invoke(st, args);
            }
            // All the rest is from the Statement interface
            if (method.getName().equals("close"))
            {
                // closing an already closed object is a no-op
                if (st == null || con.isClosed())
                    return null;

                try {
                    st.close();
                } finally {
                    con = null;
                    st = null;
                }
                return null;
            }
            if (st == null || con.isClosed())
            {
                throw new SQLException("Statement has been closed");
            }
            else if (method.getName().equals("getConnection"))
            {
                return con.getProxy(); // the proxied connection, not a physical connection
            }
            else
            {
                try
                {
                    return method.invoke(st, args);
                }
                catch (InvocationTargetException e)
                {
                    throw e.getTargetException();
                }
            }
        }
    }
}
