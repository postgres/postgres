/**
 * Redistribution and use of this software and associated documentation
 * ("Software"), with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * 1. Redistributions of source code must retain copyright
 *    statements and notices.  Redistributions must also contain a
 *    copy of this document.
 *
 * 2. Redistributions in binary form must reproduce the
 *    above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. The name "Exolab" must not be used to endorse or promote
 *    products derived from this Software without prior written
 *    permission of Exoffice Technologies.  For written permission,
 *    please contact info@exolab.org.
 *
 * 4. Products derived from this Software may not be called "Exolab"
 *    nor may "Exolab" appear in their names without prior written
 *    permission of Exoffice Technologies. Exolab is a registered
 *    trademark of Exoffice Technologies.
 *
 * 5. Due credit should be given to the Exolab Project
 *    (http://www.exolab.org/).
 *
 * THIS SOFTWARE IS PROVIDED BY EXOFFICE TECHNOLOGIES AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * EXOFFICE TECHNOLOGIES OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Copyright 1999 (C) Exoffice Technologies Inc. All Rights Reserved.
 *
 * $Id: PostgresqlDataSource.java,v 1.1 2000/10/12 08:55:24 peter Exp $
 */


package org.postgresql;


import java.io.PrintWriter;
import java.io.Serializable;
import java.util.Properties;
import java.util.Hashtable;
import java.sql.Connection;
import java.sql.SQLException;
import java.sql.DriverManager;
import java.rmi.Remote;
import javax.sql.DataSource;
import javax.naming.Referenceable;
import javax.naming.Reference;
import javax.naming.StringRefAddr;
import javax.naming.RefAddr;
import javax.naming.Context;
import javax.naming.Name;
import javax.naming.NamingException;
import javax.naming.spi.ObjectFactory;
import postgresql.util.PSQLException;
import postgresql.xa.XADataSourceImpl;


/**
 * Implements a JDBC 2.0 {@link javax.sql.DataSource} for the
 * PostgreSQL driver with JNDI persistance support. XA and pooled
 * connection support is also available, but the application must
 * used the designated DataSource interface to obtain them.
 * <p>
 * The supported data source properties are:
 * <pre>
 * description         (optional)
 * databaseName        (required)
 * loginTimeout        (optional)
 * user                (optional)
 * password            (optional)
 * serverName          (optional)
 * portNumber          (optional)
 * transactionTimeout  (optional for XA connections)
 * </pre>
 * This data source may be serialized and stored in a JNDI
 * directory. Example of how to create a new data source and
 * register it with JNDI:
 * <pre>
 * PostgresqlDataSource ds;
 * InitialContext       ctx;
 *
 * ds = new PostgresqlDataSource();
 * ds.setDatabaseName( "test" );
 * ds.setUser( "me" );
 * ds.setPassword( "secret" );
 * ctx = new InitialContext();
 * ctx.rebind( "/comp/jdbc/test", ds );
 * </pre>
 * Example for obtaining the data source from JNDI and
 * opening a new connections:
 * <pre>
 * InitialContext       ctx;
 * DataSource           ds;
 * 
 * ctx = new InitialContext();
 * ds = (DataSource) ctx.lookup( "/comp/jdbc/test" );
 * ds.getConnection();
 * </pre>
 *
 *
 * @author <a href="arkin@exoffice.com">Assaf Arkin</a>
 * @version 1.0
 * @see XADataSourceImpl
 * @see DataSource
 * @see Connection
 */
public class PostgresqlDataSource
    extends XADataSourceImpl
    implements DataSource, Referenceable,
	       ObjectFactory, Serializable
{


    /**
     * Holds the timeout for opening a new connection, specified
     * in seconds. The default is obtained from the JDBC driver.
     */
    private int _loginTimeout;


    /**
     * Holds the user's account name.
     */
    private String _user;


    /**
     * Holds the database password.
     */
    private String _password;


    /**
     * Holds the name of the particular database on the server.
     */
    private String _databaseName;


    /**
     * Description of this datasource.
     */
    private String _description = "PostgreSQL DataSource";


    /**
     * Holds the database server name. If null, this is
     * assumed to be the localhost.
     */
    private String _serverName;


    /**
     * Holds the port number where a server is listening.
     * The default value will open a connection with an
     * unspecified port.
     */
    private int _portNumber = DEFAULT_PORT;


    /**
     * The default port number. Since we open the connection
     * without specifying the port if it's the default one,
     * this value can be meaningless.
     */
    private static final int DEFAULT_PORT = 0;


    /**
     * Holds the log writer to which all messages should be
     * printed. The default writer is obtained from the driver
     * manager, but it can be specified at the datasource level
     * and will be passed to the driver. May be null.
     */    
    private transient PrintWriter _logWriter;


    /**
     * Each datasource maintains it's own driver, in case of
     * driver-specific setup (e.g. pools, log writer).
     */
    private transient postgresql.Driver _driver;




    public PostgresqlDataSource()
    {
	_logWriter = DriverManager.getLogWriter();
	_loginTimeout = DriverManager.getLoginTimeout();
    }

    
    public Connection getConnection()
        throws SQLException
    {
	// Uses the username and password specified for the datasource.
	return getConnection( _user, _password );
    }


    public synchronized Connection getConnection( String user, String password )
        throws SQLException
    {
	Connection conn;
	Properties info;
	String     url;

	if ( _driver == null ) {
	    try {
		// Constructs a driver for use just by this data source
		// which will produce TwoPhaseConnection-s. This driver
		// is not registered with the driver manager.
		_driver = new postgresql.Driver();
		_driver.setLogWriter( _logWriter );
	    } catch ( SQLException except ) {
		if ( _logWriter != null )
		    _logWriter.println( "DataSource: Failed to initialize JDBC driver: " + except );
		throw except;
	    }
	}

	// Use info to supply properties that are not in the URL.
	info = new Properties();
	info.put( "loginTimeout", Integer.toString( _loginTimeout ) );

	// DriverManager will do that and not rely on the URL alone.
	if ( user == null ) {
	    user = _user;
	    password = _password;
	}
	if ( user == null || password == null )
	    throw new PSQLException( "postgresql.ds.userpswd" );
	info.put( "user", user );
	info.put( "password", password );

        if ( _serverName != null )
            info.put( "PGHOST", _serverName );
        if ( _portNumber != DEFAULT_PORT )
            info.put( "PGPORT", Integer.toString( _portNumber ) );
        if ( _databaseName != null )
            info.put( "PGDBNAME", _databaseName );

	// Construct the URL suitable for this driver.
	url = "jdbc:postgresql:";

	// Attempt to establish a connection. Report a successful
	// attempt or a failure.
	try {
	    conn = _driver.connect( url, info );
	    if ( ! ( conn instanceof postgresql.jdbc2.Connection ) ) {
		if ( _logWriter != null )
		    _logWriter.println( "DataSource: JDBC 1 connections not supported" );
		throw new PSQLException( "postgresql.ds.onlyjdbc2" );
	    }
	} catch ( SQLException except ) {
	    if ( _logWriter != null )
		_logWriter.println( "DataSource: getConnection failed " + except );
	    throw except;
	}
	if ( conn != null && _logWriter != null )
	    _logWriter.println( "DataSource: getConnection returning " + conn );
	return conn;
    }


    public PrintWriter getLogWriter()
    {
	return _logWriter;
    }


    public synchronized void setLogWriter( PrintWriter writer )
    {
	// Once a log writer has been set, we cannot set it since some
	// thread might be conditionally accessing it right now without
	// synchronizing.
	if ( writer != null ) {
	    if ( _driver != null )
		_driver.setLogWriter( writer );
	    _logWriter = writer;
	}
    }


    public void setLoginTimeout( int seconds )
    {
	_loginTimeout = seconds;
    }


    public synchronized int getLoginTimeout()
    {
	return _loginTimeout;
    }


    /**
     * Sets the name of the particular database on the server.
     * The standard name for this property is <tt>databaseName</tt>.
     *
     * @param databaseName The name of the particular database on the server
     */
    public synchronized void setDatabaseName( String databaseName )
    {
	if ( databaseName == null )
	    throw new NullPointerException( "DataSource: Argument 'databaseName' is null" );
	_databaseName = databaseName;
    }


    /**
     * Returns the name of the particular database on the server.
     * The standard name for this property is <tt>databaseName</tt>.
     *
     * @return The name of the particular database on the server
     */
    public String getDatabaseName()
    {
	return _databaseName;
    }


    /**
     * Sets the description of this datasource.
     * The standard name for this property is <tt>description</tt>.
     *
     * @param description The description of this datasource
     */
    public synchronized void setDescription( String description )
    {
	if ( description == null )
	    throw new NullPointerException( "DataSource: Argument 'description' is null" );
	_description = description;
    }


    /**
     * Returns the description of this datasource.
     * The standard name for this property is <tt>description</tt>.
     *
     * @return The description of this datasource
     */
    public String getDescription()
    {
	return _description;
    }


    /**
     * Sets the database password.
     * The standard name for this property is <tt>password</tt>.
     *
     * @param password The database password
     */
    public synchronized void setPassword( String password )
    {
	_password = password;
    }


    /**
     * Returns the database password.
     * The standard name for this property is <tt>password</tt>.
     *
     * @return The database password
     */
    public String getPassword()
    {
	return _password;
    }


    /**
     * Sets the port number where a server is listening.
     * The standard name for this property is <tt>portNumber</tt>.
     *
     * @param portNumber The port number where a server is listening
     */
    public synchronized void setPortNumber( int portNumber )
    {
	_portNumber = portNumber;
    }


    /**
     * Returns the port number where a server is listening.
     * The standard name for this property is <tt>portNumber</tt>.
     *
     * @return The port number where a server is listening
     */
    public int getPortNumber()
    {
	return _portNumber;
    }


    /**
     * Sets the database server name.

     * The standard name for this property is <tt>serverName</tt>.
     *
     * @param serverName The database server name
     */
    public synchronized void setServerName( String serverName )
    {
	_serverName = serverName;
    }


    /**
     * Returns the database server name.
     * The standard name for this property is <tt>serverName</tt>.
     *
     * @return The database server name
     */
    public String getServerName()
    {
	return _serverName;
    }


    /**
     * Sets the user's account name.
     * The standard name for this property is <tt>user</tt>.
     *
     * @param user The user's account name
     */
    public synchronized void setUser( String user )
    {
	_user = user;
    }


    /**
     * Returns the user's account name.
     * The standard name for this property is <tt>user</tt>.
     *
     * @return The user's account name
     */
    public String getUser()
    {
	return _user;
    }


    /**
     * Returns true if this datasource and the other are equal.
     * The two datasources are equal if and only if they will produce
     * the exact same connections. Connection properties like database
     * name, user name, etc are comapred. Setup properties like
     * description, log writer, etc are not compared.
     */
    public synchronized boolean equals( Object other )
    {
	if ( other == this )
	    return true;
	if ( other == null || ! ( other instanceof PostgresqlDataSource ) )
	    return false;

	PostgresqlDataSource with;

	with = (PostgresqlDataSource) other;
	if ( _databaseName != null && _databaseName.equals( with._databaseName ) )
	    if ( _portNumber == with._portNumber && 
		 ( ( _serverName == null && with._serverName == null ) ||
		   ( _serverName != null && _serverName.equals( with._serverName ) ) ) )
		if ( ( _user == null && with._user == null ) ||
		     ( _user != null && _password != null && _user.equals( with._user ) &&
		       _password.equals( with._password ) ) )
		    return true;
	return false;
    }


    public String toString()
    {
	if ( _description != null )
	    return _description;
	else {
	    String url;
	    
	    url = "jdbc:postgresql:";
	    if ( _serverName != null ) {
		if ( _portNumber == DEFAULT_PORT )
		    url = url + "//" + _serverName + "/";
		else
		    url = url + "//" + _serverName + ":" + _portNumber + "/";
	    } else if ( _portNumber != DEFAULT_PORT )
		url = url + "//localhost:" + _portNumber + "/";
	    if ( _databaseName != null )
		url = url + _databaseName;
	    return "DataSource " + url;
	}
    }


    public synchronized Reference getReference()
    {
	Reference ref;

	// We use same object as factory.
	ref = new Reference( getClass().getName(), getClass().getName(), null );
	// Mandatory properties
	ref.add( new StringRefAddr( "description", _description ) );
	ref.add( new StringRefAddr( "databaseName", _databaseName ) );
	ref.add( new StringRefAddr( "loginTimeout", Integer.toString( _loginTimeout ) ) );
	// Optional properties
	if ( _user != null )
	    ref.add( new StringRefAddr( "user", _user ) );
	if ( _password != null )
	    ref.add( new StringRefAddr( "password", _password ) );
	if ( _serverName != null )
	    ref.add( new StringRefAddr( "serverName", _serverName ) );
	if ( _portNumber != DEFAULT_PORT )
	    ref.add( new StringRefAddr( "portNumber", Integer.toString( _portNumber ) ) );
	ref.add( new StringRefAddr( "transactionTimeout", Integer.toString( getTransactionTimeout() ) ) );
 	return ref;
    }


    public Object getObjectInstance( Object refObj, Name name, Context nameCtx, Hashtable env )
        throws NamingException
    {
	Reference ref;

	// Can only reconstruct from a reference.
	if ( refObj instanceof Reference ) {
	    ref = (Reference) refObj;
	    // Make sure reference is of datasource class.
	    if ( ref.getClassName().equals( getClass().getName() ) ) {

		PostgresqlDataSource ds;
		RefAddr              addr;

		try {
		    ds = (PostgresqlDataSource) Class.forName( ref.getClassName() ).newInstance();
		} catch ( Exception except ) {
		    throw new NamingException( except.toString() );
		}
		// Mandatory properties
		ds._description = (String) ref.get( "description" ).getContent();
		ds._databaseName = (String) ref.get( "databaseName" ).getContent();
		ds._loginTimeout = Integer.parseInt( (String) ref.get( "loginTimeout" ).getContent() );
		// Optional properties
		addr = ref.get( "user" );
		if ( addr != null )
		    ds._user = (String) addr.getContent();
		addr = ref.get( "password" );
		if ( addr != null )
		    ds._password = (String) addr.getContent();
		addr = ref.get( "serverName" );
		if ( addr != null )
		    ds._serverName = (String) addr.getContent();
		addr = ref.get( "portNumber" );
		if ( addr != null )
		    ds._portNumber = Integer.parseInt( (String) addr.getContent() );
		addr = ref.get( "transactionTimeout" );
		if ( addr != null )
		    setTransactionTimeout( Integer.parseInt( (String) addr.getContent() ) );
		return ds;

	    } else
		throw new NamingException( "DataSource: Reference not constructed from class " + getClass().getName() );
	} else if ( refObj instanceof Remote )
	    return refObj;
	else
	    return null;
    }


}

