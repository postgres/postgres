package org.postgresql.jdbc2.optional;

import javax.naming.*;
import java.io.PrintWriter;
import java.sql.*;

import java.io.ObjectOutputStream;
import java.io.ObjectInputStream;
import java.io.IOException;

/**
 * Base class for data sources and related classes.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.3.6.1 $
 */
public abstract class BaseDataSource implements Referenceable
{
	// Load the normal driver, since we'll use it to actually connect to the
	// database.  That way we don't have to maintain the connecting code in
	// multiple places.
	static {
		try
		{
			Class.forName("org.postgresql.Driver");
		}
		catch (ClassNotFoundException e)
		{
			System.err.println("PostgreSQL DataSource unable to load PostgreSQL JDBC Driver");
		}
	}

	// Needed to implement the DataSource/ConnectionPoolDataSource interfaces
	private transient PrintWriter logger;
	// Don't track loginTimeout, since we'd just ignore it anyway

	// Standard properties, defined in the JDBC 2.0 Optional Package spec
	private String serverName = "localhost";
	private String databaseName;
	private String user;
	private String password;
	private int portNumber;

	/**
	 * Gets a connection to the PostgreSQL database.  The database is identified by the
	 * DataSource properties serverName, databaseName, and portNumber.	The user to
	 * connect as is identified by the DataSource properties user and password.
	 *
	 * @return A valid database connection.
	 * @throws SQLException
	 *		   Occurs when the database connection cannot be established.
	 */
	public Connection getConnection() throws SQLException
	{
		return getConnection(user, password);
	}

	/**
	 * Gets a connection to the PostgreSQL database.  The database is identified by the
	 * DataSource properties serverName, databaseName, and portNumber.	The user to
	 * connect as is identified by the arguments user and password, which override
	 * the DataSource properties by the same name.
	 *
	 * @return A valid database connection.
	 * @throws SQLException
	 *		   Occurs when the database connection cannot be established.
	 */
	public Connection getConnection(String user, String password) throws SQLException
	{
		try
		{
			Connection con = DriverManager.getConnection(getUrl(), user, password);
			if (logger != null)
			{
				logger.println("Created a non-pooled connection for " + user + " at " + getUrl());
			}
			return con;
		}
		catch (SQLException e)
		{
			if (logger != null)
			{
				logger.println("Failed to create a non-pooled connection for " + user + " at " + getUrl() + ": " + e);
			}
			throw e;
		}
	}

	/**
	 * This DataSource does not support a configurable login timeout.
	 * @return 0
	 */
	public int getLoginTimeout() throws SQLException
	{
		return 0;
	}

	/**
	 * This DataSource does not support a configurable login timeout.  Any value
	 * provided here will be ignored.
	 */
	public void setLoginTimeout(int i) throws SQLException
		{}

	/**
	 * Gets the log writer used to log connections opened.
	 */
	public PrintWriter getLogWriter() throws SQLException
	{
		return logger;
	}

	/**
	 * The DataSource will note every connection opened to the provided log writer.
	 */
	public void setLogWriter(PrintWriter printWriter) throws SQLException
	{
		logger = printWriter;
	}

	/**
	 * Gets the name of the host the PostgreSQL database is running on.
	 */
	public String getServerName()
	{
		return serverName;
	}

	/**
	 * Sets the name of the host the PostgreSQL database is running on.  If this
	 * is changed, it will only affect future calls to getConnection.  The default
	 * value is <tt>localhost</tt>.
	 */
	public void setServerName(String serverName)
	{
		if (serverName == null || serverName.equals(""))
		{
			this.serverName = "localhost";
		}
		else
		{
			this.serverName = serverName;
		}
	}

	/**
	 * Gets the name of the PostgreSQL database, running on the server identified
	 * by the serverName property.
	 */
	public String getDatabaseName()
	{
		return databaseName;
	}

	/**
	 * Sets the name of the PostgreSQL database, running on the server identified
	 * by the serverName property.	If this is changed, it will only affect
	 * future calls to getConnection.
	 */
	public void setDatabaseName(String databaseName)
	{
		this.databaseName = databaseName;
	}

	/**
	 * Gets a description of this DataSource-ish thing.  Must be customized by
	 * subclasses.
	 */
	public abstract String getDescription();

	/**
	 * Gets the user to connect as by default.	If this is not specified, you must
	 * use the getConnection method which takes a user and password as parameters.
	 */
	public String getUser()
	{
		return user;
	}

	/**
	 * Sets the user to connect as by default.	If this is not specified, you must
	 * use the getConnection method which takes a user and password as parameters.
	 * If this is changed, it will only affect future calls to getConnection.
	 */
	public void setUser(String user)
	{
		this.user = user;
	}

	/**
	 * Gets the password to connect with by default.  If this is not specified but a
	 * password is needed to log in, you must use the getConnection method which takes
	 * a user and password as parameters.
	 */
	public String getPassword()
	{
		return password;
	}

	/**
	 * Sets the password to connect with by default.  If this is not specified but a
	 * password is needed to log in, you must use the getConnection method which takes
	 * a user and password as parameters.  If this is changed, it will only affect
	 * future calls to getConnection.
	 */
	public void setPassword(String password)
	{
		this.password = password;
	}

	/**
	 * Gets the port which the PostgreSQL server is listening on for TCP/IP
	 * connections.
	 *
	 * @return The port, or 0 if the default port will be used.
	 */
	public int getPortNumber()
	{
		return portNumber;
	}

	/**
	 * Gets the port which the PostgreSQL server is listening on for TCP/IP
	 * connections.  Be sure the -i flag is passed to postmaster when PostgreSQL
	 * is started.	If this is not set, or set to 0, the default port will be used.
	 */
	public void setPortNumber(int portNumber)
	{
		this.portNumber = portNumber;
	}

	/**
	 * Generates a DriverManager URL from the other properties supplied.
	 */
	private String getUrl()
	{
		return "jdbc:postgresql://" + serverName + (portNumber == 0 ? "" : ":" + portNumber) + "/" + databaseName;
	}

    /**
     * Generates a reference using the appropriate object factory.  This
     * implementation uses the JDBC 2 optional package object factory.
     */
    protected Reference createReference()
    {
        return new Reference(getClass().getName(), PGObjectFactory.class.getName(), null);
    }

	public Reference getReference() throws NamingException
	{
		Reference ref = createReference();
		ref.add(new StringRefAddr("serverName", serverName));
		if (portNumber != 0)
		{
			ref.add(new StringRefAddr("portNumber", Integer.toString(portNumber)));
		}
		ref.add(new StringRefAddr("databaseName", databaseName));
		if (user != null)
		{
			ref.add(new StringRefAddr("user", user));
		}
		if (password != null)
		{
			ref.add(new StringRefAddr("password", password));
		}
		return ref;
	}

	protected void writeBaseObject(ObjectOutputStream out) throws IOException
	{
		out.writeObject(serverName);
		out.writeObject(databaseName);
		out.writeObject(user);
		out.writeObject(password);
		out.writeInt(portNumber);
	}

	protected void readBaseObject(ObjectInputStream in) throws IOException, ClassNotFoundException
	{
		serverName = (String)in.readObject();
		databaseName = (String)in.readObject();
		user = (String)in.readObject();
		password = (String)in.readObject();
		portNumber = in.readInt();
	}

}
