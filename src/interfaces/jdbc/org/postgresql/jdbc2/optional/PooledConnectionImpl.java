package org.postgresql.jdbc2.optional;

import javax.sql.*;
import java.sql.SQLException;
import java.sql.Connection;
import java.util.*;
import java.lang.reflect.*;

/**
 * PostgreSQL implementation of the PooledConnection interface.  This shouldn't
 * be used directly, as the pooling client should just interact with the
 * ConnectionPool instead.
 * @see ConnectionPool
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.3 $
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
			throw new SQLException("This PooledConnection has already been closed!");
		}
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
		ConnectionHandler handler = new ConnectionHandler(con);
		last = handler;
		return (Connection)Proxy.newProxyInstance(getClass().getClassLoader(), new Class[]{Connection.class}, handler);
	}

	/**
	 * Used to fire a connection event to all listeners.
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
	 * Used to fire a connection event to all listeners.
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
				return method.invoke(con, args);
			}
			// All the rest is from the Connection interface
			if (method.getName().equals("isClosed"))
			{
				return con == null ? Boolean.TRUE : Boolean.FALSE;
			}
			if (con == null)
			{
				throw new SQLException(automatic ? "Connection has been closed automatically because a new connection was opened for the same PooledConnection or the PooledConnection has been closed" : "Connection has been closed");
			}
			if (method.getName().equals("close"))
			{
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
				last = null;
				fireConnectionClosed();
				if (ex != null)
				{
					throw ex;
				}
				return null;
			}
			else
			{
				return method.invoke(con, args);
			}
		}

		public void close()
		{
			if (con != null)
			{
				automatic = true;
			}
			con = null;
			// No close event fired here: see JDBC 2.0 Optional Package spec section 6.3
		}
	}
}
