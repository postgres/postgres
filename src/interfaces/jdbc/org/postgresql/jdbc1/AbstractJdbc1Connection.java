/*-------------------------------------------------------------------------
 *
 * AbstractJdbc1Connection.java
 *     This class defines methods of the jdbc1 specification.  This class is
 *     extended by org.postgresql.jdbc2.AbstractJdbc2Connection which adds 
 *     the jdbc2 methods.  The real Connection class (for jdbc1) is 
 *     org.postgresql.jdbc1.Jdbc1Connection
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/AbstractJdbc1Connection.java,v 1.27.2.4 2004/08/11 06:56:00 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.jdbc1;


import java.io.IOException;
import java.net.ConnectException;
import java.sql.*;
import java.util.*;
import org.postgresql.Driver;
import org.postgresql.PGNotification;
import org.postgresql.core.BaseConnection;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Encoding;
import org.postgresql.core.PGStream;
import org.postgresql.core.QueryExecutor;
import org.postgresql.core.StartupPacket;
import org.postgresql.fastpath.Fastpath;
import org.postgresql.largeobject.LargeObjectManager;
import org.postgresql.util.MD5Digest;
import org.postgresql.util.PGobject;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;
import org.postgresql.util.UnixCrypt;

public abstract class AbstractJdbc1Connection implements BaseConnection
{
	// This is the network stream associated with this connection
	private PGStream pgStream;

	public PGStream getPGStream() {
		return pgStream;
	}
  
	protected String PG_HOST;
	protected int PG_PORT;
	protected String PG_USER;
	protected String PG_DATABASE;
	protected boolean PG_STATUS;
	protected String compatible;
	protected boolean useSSL;

	// The PID an cancellation key we get from the backend process
	protected int pid;
	protected int ckey;

	private Vector m_notifications;

	/*
	 The encoding to use for this connection.
	 */
	private Encoding encoding = Encoding.defaultEncoding();

	private String dbVersionNumber;

	public boolean CONNECTION_OK = true;
	public boolean CONNECTION_BAD = false;

	public boolean autoCommit = true;
	public boolean readOnly = false;

	public Driver this_driver;
	private String this_url;
	private String cursor = null;	// The positioned update cursor name

	private int PGProtocolVersionMajor = 2;
	private int PGProtocolVersionMinor = 0;
	public int getPGProtocolVersionMajor() { return PGProtocolVersionMajor; }
	public int getPGProtocolVersionMinor() { return PGProtocolVersionMinor; }

	private static final int AUTH_REQ_OK = 0;
	private static final int AUTH_REQ_KRB4 = 1;
	private static final int AUTH_REQ_KRB5 = 2;
	private static final int AUTH_REQ_PASSWORD = 3;
	private static final int AUTH_REQ_CRYPT = 4;
	private static final int AUTH_REQ_MD5 = 5;
	private static final int AUTH_REQ_SCM = 6;


	// These are used to cache oids, PGTypes and SQLTypes
	private static Hashtable sqlTypeCache = new Hashtable();  // oid -> SQLType
	private static Hashtable pgTypeCache = new Hashtable();  // oid -> PGType
	private static Hashtable typeOidCache = new Hashtable();  //PGType -> oid

	// Now handle notices as warnings, so things like "show" now work
	public SQLWarning firstWarning = null;

	/*
	 * Cache of the current isolation level
	 */
	private int isolationLevel = Connection.TRANSACTION_READ_COMMITTED;


	public abstract Statement createStatement() throws SQLException;
	public abstract DatabaseMetaData getMetaData() throws SQLException;

	/*
	 * This method actually opens the connection. It is called by Driver.
	 *
	 * @param host the hostname of the database back end
	 * @param port the port number of the postmaster process
	 * @param info a Properties[] thing of the user and password
	 * @param database the database to connect to
	 * @param url the URL of the connection
	 * @param d the Driver instantation of the connection
	 * @exception SQLException if a database access error occurs
	 */
	public void openConnection(String host, int port, Properties info, String database, String url, Driver d) throws SQLException
	  {
		firstWarning = null;

		// Throw an exception if the user or password properties are missing
		// This occasionally occurs when the client uses the properties version
		// of getConnection(), and is a common question on the email lists
		if (info.getProperty("user") == null)
			throw new PSQLException("postgresql.con.user", PSQLState.CONNECTION_REJECTED);

		this_driver = (Driver)d;
		this_url = url;

		PG_DATABASE = database;
		PG_USER = info.getProperty("user");

		String password = info.getProperty("password", "");
		PG_PORT = port;

		PG_HOST = host;
		PG_STATUS = CONNECTION_BAD;

		if (info.getProperty("ssl") != null && Driver.sslEnabled())
		{
			useSSL = true;
		}
		else
		{
			useSSL = false;
		}

		if (info.getProperty("compatible") == null)
		{
			compatible = d.getMajorVersion() + "." + d.getMinorVersion();
		}
		else
		{
			compatible = info.getProperty("compatible");
		}

		//Read loglevel arg and set the loglevel based on this value
		//in addition to setting the log level enable output to
		//standard out if no other printwriter is set
		String l_logLevelProp = info.getProperty("loglevel", "0");
		int l_logLevel = 0;
		try
		{
			l_logLevel = Integer.parseInt(l_logLevelProp);
			if (l_logLevel > Driver.DEBUG || l_logLevel < Driver.INFO)
			{
				l_logLevel = 0;
			}
		}
		catch (Exception l_e)
		{
			//invalid value for loglevel ignore
		}
		if (l_logLevel > 0)
		{
			Driver.setLogLevel(l_logLevel);
			enableDriverManagerLogging();
		}

		//Print out the driver version number
		if (Driver.logInfo)
			Driver.info(Driver.getVersion());
		if (Driver.logDebug) {
			Driver.debug("    ssl = " + useSSL);
			Driver.debug("    compatible = " + compatible);
			Driver.debug("    loglevel = " + l_logLevel);
		}

		// Now make the initial connection
		try
		{
			pgStream = new PGStream(host, port);
		}
		catch (ConnectException cex)
		{
			// Added by Peter Mount <peter@retep.org.uk>
			// ConnectException is thrown when the connection cannot be made.
			// we trap this an return a more meaningful message for the end user
			throw new PSQLException ("postgresql.con.refused", PSQLState.CONNECTION_REJECTED);
		}
		catch (IOException e)
		{
			throw new PSQLException ("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}

		try {
			//Now do the protocol work
			if (haveMinimumCompatibleVersion("7.4")) {
				openConnectionV3(host,port,info,database,url,d,password);
			} else {
				openConnectionV2(host,port,info,database,url,d,password);
			}
		} catch (SQLException sqle) {
			// if we fail to completely establish a connection,
			// close down the socket to not leak resources.
			try {
				pgStream.close();
			} catch (IOException ioe) { }

			throw sqle;
		}
	  }

	private void openConnectionV3(String p_host, int p_port, Properties p_info, String p_database, String p_url, Driver p_d, String p_password) throws SQLException
	  {
		PGProtocolVersionMajor = 3;
		if (Driver.logDebug)
			Driver.debug("Using Protocol Version3");

		// Now we need to construct and send an ssl startup packet
		try
		{
			if (useSSL) {
				if (Driver.logDebug)
					Driver.debug("Asking server if it supports ssl");
				pgStream.SendInteger(8,4);
				pgStream.SendInteger(80877103,4);

				// now flush the ssl packets to the backend
				pgStream.flush();

				// Now get the response from the backend, either an error message
				// or an authentication request
				int beresp = pgStream.ReceiveChar();
				if (Driver.logDebug)
					Driver.debug("Server response was (S=Yes,N=No): "+(char)beresp);
				switch (beresp)
					{
					case 'E':
						// An error occured, so pass the error message to the
						// user.
						//
						// The most common one to be thrown here is:
						// "User authentication failed"
						//
						throw new PSQLException("postgresql.con.misc", PSQLState.CONNECTION_REJECTED, pgStream.ReceiveString(encoding));
						
					case 'N':
						// Server does not support ssl
						throw new PSQLException("postgresql.con.sslnotsupported", PSQLState.CONNECTION_FAILURE);
						
					case 'S':
						// Server supports ssl
						if (Driver.logDebug)
							Driver.debug("server does support ssl");
						Driver.makeSSL(pgStream);
						break;

					default:
						throw new PSQLException("postgresql.con.sslfail", PSQLState.CONNECTION_FAILURE);
					}
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}


		// Now we need to construct and send a startup packet
		try
		{
			new StartupPacket(PGProtocolVersionMajor,
							  PGProtocolVersionMinor,
							  PG_USER,
							  p_database).writeTo(pgStream);

			// now flush the startup packets to the backend
			pgStream.flush();

			// Now get the response from the backend, either an error message
			// or an authentication request
			int areq = -1; // must have a value here
			do
			{
				int beresp = pgStream.ReceiveChar();
				String salt = null;
				byte [] md5Salt = new byte[4];
				switch (beresp)
				{
					case 'E':
						// An error occured, so pass the error message to the
						// user.
						//
						// The most common one to be thrown here is:
						// "User authentication failed"
						//
						int l_elen = pgStream.ReceiveIntegerR(4);
						if (l_elen > 30000) {
							//if the error length is > than 30000 we assume this is really a v2 protocol 
							//server so try again with a v2 connection
							//need to create a new connection and try again
							pgStream.close();
							try
							{
								pgStream = new PGStream(p_host, p_port);
							}
							catch (ConnectException cex)
							{
								// Added by Peter Mount <peter@retep.org.uk>
								// ConnectException is thrown when the connection cannot be made.
								// we trap this an return a more meaningful message for the end user
								throw new PSQLException ("postgresql.con.refused", PSQLState.CONNECTION_REJECTED);
							}
							catch (IOException e)
							{
								throw new PSQLException ("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
							}
							openConnectionV2(p_host, p_port, p_info, p_database, p_url, p_d, p_password);
							return;
						}
						throw new PSQLException("postgresql.con.misc", PSQLState.CONNECTION_REJECTED, PSQLException.parseServerError(encoding.decode(pgStream.Receive(l_elen-4))));

					case 'R':
						// Get the message length
						int l_msgLen = pgStream.ReceiveIntegerR(4);
						// Get the type of request
						areq = pgStream.ReceiveIntegerR(4);
						// Get the crypt password salt if there is one
						if (areq == AUTH_REQ_CRYPT)
						{
							byte[] rst = new byte[2];
							rst[0] = (byte)pgStream.ReceiveChar();
							rst[1] = (byte)pgStream.ReceiveChar();
							salt = new String(rst, 0, 2);
							if (Driver.logDebug)
								Driver.debug("Crypt salt=" + salt);
						}

						// Or get the md5 password salt if there is one
						if (areq == AUTH_REQ_MD5)
						{

							md5Salt[0] = (byte)pgStream.ReceiveChar();
							md5Salt[1] = (byte)pgStream.ReceiveChar();
							md5Salt[2] = (byte)pgStream.ReceiveChar();
							md5Salt[3] = (byte)pgStream.ReceiveChar();
							if (Driver.logDebug) {
								String md5SaltString = "";
								for (int i=0; i<md5Salt.length; i++) {
									md5SaltString += " " + md5Salt[i];
								}
								Driver.debug("MD5 salt=" + md5SaltString);
							}
						}

						// now send the auth packet
						switch (areq)
						{
							case AUTH_REQ_OK:
								break;

							case AUTH_REQ_KRB4:
								if (Driver.logDebug)
									Driver.debug("postgresql: KRB4");
								throw new PSQLException("postgresql.con.kerb4", PSQLState.CONNECTION_REJECTED);

							case AUTH_REQ_KRB5:
								if (Driver.logDebug)
									Driver.debug("postgresql: KRB5");
								throw new PSQLException("postgresql.con.kerb5", PSQLState.CONNECTION_REJECTED);

							case AUTH_REQ_SCM:
								if (Driver.logDebug)
									Driver.debug("postgresql: SCM");
								throw new PSQLException("postgresql.con.scm", PSQLState.CONNECTION_REJECTED);


							case AUTH_REQ_PASSWORD:
								if (Driver.logDebug)
									Driver.debug("postgresql: PASSWORD");
								pgStream.SendChar('p');
								pgStream.SendInteger(5 + p_password.length(), 4);
								pgStream.Send(p_password.getBytes());
								pgStream.SendChar(0);
								pgStream.flush();
								break;

							case AUTH_REQ_CRYPT:
								if (Driver.logDebug)
									Driver.debug("postgresql: CRYPT");
								String crypted = UnixCrypt.crypt(salt, p_password);
								pgStream.SendChar('p');
								pgStream.SendInteger(5 + crypted.length(), 4);
								pgStream.Send(crypted.getBytes());
								pgStream.SendChar(0);
								pgStream.flush();
								break;

							case AUTH_REQ_MD5:
								if (Driver.logDebug)
									Driver.debug("postgresql: MD5");
								byte[] digest = MD5Digest.encode(PG_USER, p_password, md5Salt);
								pgStream.SendChar('p');
								pgStream.SendInteger(5 + digest.length, 4);
								pgStream.Send(digest);
								pgStream.SendChar(0);
								pgStream.flush();
								break;

							default:
								throw new PSQLException("postgresql.con.auth", PSQLState.CONNECTION_REJECTED, new Integer(areq));
						}
						break;

					default:
						throw new PSQLException("postgresql.con.authfail", PSQLState.CONNECTION_REJECTED);
				}
			}
			while (areq != AUTH_REQ_OK);

		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}

		int beresp;
		do
		{
			beresp = pgStream.ReceiveChar();
			switch (beresp)
			{
			    case 'Z':
					//ready for query
					break;
				case 'K':
					int l_msgLen = pgStream.ReceiveIntegerR(4);
					if (l_msgLen != 12) throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
					pid = pgStream.ReceiveIntegerR(4);
					ckey = pgStream.ReceiveIntegerR(4);
					break;
				case 'E':
					int l_elen = pgStream.ReceiveIntegerR(4);
					throw new PSQLException("postgresql.con.backend", PSQLState.CONNECTION_UNABLE_TO_CONNECT, PSQLException.parseServerError(encoding.decode(pgStream.Receive(l_elen-4))));
				case 'N':
					int l_nlen = pgStream.ReceiveIntegerR(4);
					PSQLException notify = PSQLException.parseServerError(encoding.decode(pgStream.Receive(l_nlen-4)));
					addWarning(notify.getMessage());
					break;
			    case 'S':
					//TODO: handle parameter status messages
					int l_len = pgStream.ReceiveIntegerR(4);
					String l_pStatus = encoding.decode(pgStream.Receive(l_len-4));
					if (Driver.logDebug)
						Driver.debug("ParameterStatus="+ l_pStatus);
					break;
				default:
					if (Driver.logDebug)
						Driver.debug("invalid state="+ (char)beresp);
					throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
			}
		}
		while (beresp != 'Z');
		// read ReadyForQuery
		if (pgStream.ReceiveIntegerR(4) != 5) throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT); 
		//TODO: handle transaction status
		char l_tStatus = (char)pgStream.ReceiveChar();

		// "pg_encoding_to_char(1)" will return 'EUC_JP' for a backend compiled with multibyte,
		// otherwise it's hardcoded to 'SQL_ASCII'.
		// If the backend doesn't know about multibyte we can't assume anything about the encoding
		// used, so we denote this with 'UNKNOWN'.
		//Note: begining with 7.2 we should be using pg_client_encoding() which
		//is new in 7.2.  However it isn't easy to conditionally call this new
		//function, since we don't yet have the information as to what server
		//version we are talking to.  Thus we will continue to call
		//getdatabaseencoding() until we drop support for 7.1 and older versions
		//or until someone comes up with a conditional way to run one or
		//the other function depending on server version that doesn't require
		//two round trips to the server per connection

		final String encodingQuery =
			"case when pg_encoding_to_char(1) = 'SQL_ASCII' then 'UNKNOWN' else getdatabaseencoding() end";

		// Set datestyle and fetch db encoding in a single call, to avoid making
		// more than one round trip to the backend during connection startup.


		BaseResultSet resultSet
			= execSQL("set datestyle to 'ISO'; select version(), " + encodingQuery + ";");
		
		if (! resultSet.next())
		{
			throw new PSQLException("postgresql.con.failed.bad.encoding", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
		}
		String version = resultSet.getString(1);
		dbVersionNumber = extractVersionNumber(version);

		String dbEncoding = resultSet.getString(2);
		encoding = Encoding.getEncoding(dbEncoding, p_info.getProperty("charSet"));
		//In 7.3 we are forced to do a second roundtrip to handle the case 
		//where a database may not be running in autocommit mode
		//jdbc by default assumes autocommit is on until setAutoCommit(false)
		//is called.  Therefore we need to ensure a new connection is 
		//initialized to autocommit on.
		//We also set the client encoding so that the driver only needs 
		//to deal with utf8.  We can only do this in 7.3 because multibyte 
		//support is now always included
		if (haveMinimumServerVersion("7.3")) 
		{
			BaseResultSet acRset =
			//TODO: if protocol V3 we can set the client encoding in startup
			execSQL("set client_encoding = 'UNICODE'");
			//set encoding to be unicode
			encoding = Encoding.getEncoding("UNICODE", null);

		}

		// Initialise object handling
		initObjectTypes();

		// Mark the connection as ok, and cleanup
		PG_STATUS = CONNECTION_OK;
	}

	private void openConnectionV2(String host, int port, Properties info, String database, String url, Driver d, String password) throws SQLException
	  {
		PGProtocolVersionMajor = 2;
		if (Driver.logDebug)
			Driver.debug("Using Protocol Version2");

		// Now we need to construct and send an ssl startup packet
		try
		{
			if (useSSL) {
				if (Driver.logDebug)
					Driver.debug("Asking server if it supports ssl");
				pgStream.SendInteger(8,4);
				pgStream.SendInteger(80877103,4);

				// now flush the ssl packets to the backend
				pgStream.flush();

				// Now get the response from the backend, either an error message
				// or an authentication request
				int beresp = pgStream.ReceiveChar();
				if (Driver.logDebug)
					Driver.debug("Server response was (S=Yes,N=No): "+(char)beresp);
				switch (beresp)
					{
					case 'E':
						// An error occured, so pass the error message to the
						// user.
						//
						// The most common one to be thrown here is:
						// "User authentication failed"
						//
						throw new PSQLException("postgresql.con.misc", PSQLState.CONNECTION_REJECTED, pgStream.ReceiveString(encoding));
						
					case 'N':
						// Server does not support ssl
						throw new PSQLException("postgresql.con.sslnotsupported", PSQLState.CONNECTION_FAILURE);
						
					case 'S':
						// Server supports ssl
						if (Driver.logDebug)
							Driver.debug("server does support ssl");
						Driver.makeSSL(pgStream);
						break;

					default:
						throw new PSQLException("postgresql.con.sslfail", PSQLState.CONNECTION_FAILURE);
					}
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}


		// Now we need to construct and send a startup packet
		try
		{
			new StartupPacket(PGProtocolVersionMajor,
							  PGProtocolVersionMinor,
							  PG_USER,
							  database).writeTo(pgStream);

			// now flush the startup packets to the backend
			pgStream.flush();

			// Now get the response from the backend, either an error message
			// or an authentication request
			int areq = -1; // must have a value here
			do
			{
				int beresp = pgStream.ReceiveChar();
				String salt = null;
				byte [] md5Salt = new byte[4];
				switch (beresp)
				{
					case 'E':
						// An error occured, so pass the error message to the
						// user.
						//
						// The most common one to be thrown here is:
						// "User authentication failed"
						//
						throw new PSQLException("postgresql.con.misc", PSQLState.CONNECTION_REJECTED, pgStream.ReceiveString(encoding));

					case 'R':
						// Get the type of request
						areq = pgStream.ReceiveIntegerR(4);
						// Get the crypt password salt if there is one
						if (areq == AUTH_REQ_CRYPT)
						{
							byte[] rst = new byte[2];
							rst[0] = (byte)pgStream.ReceiveChar();
							rst[1] = (byte)pgStream.ReceiveChar();
							salt = new String(rst, 0, 2);
							if (Driver.logDebug)
								Driver.debug("Crypt salt=" + salt);
						}

						// Or get the md5 password salt if there is one
						if (areq == AUTH_REQ_MD5)
						{

							md5Salt[0] = (byte)pgStream.ReceiveChar();
							md5Salt[1] = (byte)pgStream.ReceiveChar();
							md5Salt[2] = (byte)pgStream.ReceiveChar();
							md5Salt[3] = (byte)pgStream.ReceiveChar();
							if (Driver.logDebug) {
								String md5SaltString = "";
								for (int i=0; i<md5Salt.length; i++) {
									md5SaltString += " " + md5Salt[i];
								}
								Driver.debug("MD5 salt=" + md5SaltString);
							}
						}

						// now send the auth packet
						switch (areq)
						{
							case AUTH_REQ_OK:
								break;

							case AUTH_REQ_KRB4:
								if (Driver.logDebug)
									Driver.debug("postgresql: KRB4");
								throw new PSQLException("postgresql.con.kerb4", PSQLState.CONNECTION_REJECTED);

							case AUTH_REQ_KRB5:
								if (Driver.logDebug)
									Driver.debug("postgresql: KRB5");
								throw new PSQLException("postgresql.con.kerb5", PSQLState.CONNECTION_REJECTED);

							case AUTH_REQ_PASSWORD:
								if (Driver.logDebug)
									Driver.debug("postgresql: PASSWORD");
								pgStream.SendInteger(5 + password.length(), 4);
								pgStream.Send(password.getBytes());
								pgStream.SendInteger(0, 1);
								pgStream.flush();
								break;

							case AUTH_REQ_CRYPT:
								if (Driver.logDebug)
									Driver.debug("postgresql: CRYPT");
								String crypted = UnixCrypt.crypt(salt, password);
								pgStream.SendInteger(5 + crypted.length(), 4);
								pgStream.Send(crypted.getBytes());
								pgStream.SendInteger(0, 1);
								pgStream.flush();
								break;

							case AUTH_REQ_MD5:
								if (Driver.logDebug)
									Driver.debug("postgresql: MD5");
								byte[] digest = MD5Digest.encode(PG_USER, password, md5Salt);
								pgStream.SendInteger(5 + digest.length, 4);
								pgStream.Send(digest);
								pgStream.SendInteger(0, 1);
								pgStream.flush();
								break;

							default:
								throw new PSQLException("postgresql.con.auth", PSQLState.CONNECTION_REJECTED, new Integer(areq));
						}
						break;

					default:
						throw new PSQLException("postgresql.con.authfail", PSQLState.CONNECTION_REJECTED);
				}
			}
			while (areq != AUTH_REQ_OK);

		}
		catch (IOException e)
		{
			//Should be passing exception as arg.
			throw new PSQLException("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}


		// As of protocol version 2.0, we should now receive the cancellation key and the pid
		int beresp;
		do
		{
			beresp = pgStream.ReceiveChar();
			switch (beresp)
			{
				case 'K':
					pid = pgStream.ReceiveIntegerR(4);
					ckey = pgStream.ReceiveIntegerR(4);
					break;
				case 'E':
					throw new PSQLException("postgresql.con.backend", PSQLState.CONNECTION_UNABLE_TO_CONNECT, pgStream.ReceiveString(encoding));
				case 'N':
					addWarning(pgStream.ReceiveString(encoding));
					break;
				default:
					throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
			}
		}
		while (beresp == 'N');

		// Expect ReadyForQuery packet
		do
		{
			beresp = pgStream.ReceiveChar();
			switch (beresp)
			{
				case 'Z':
					break;
				case 'N':
					addWarning(pgStream.ReceiveString(encoding));
					break;
				case 'E':
					throw new PSQLException("postgresql.con.backend", PSQLState.CONNECTION_UNABLE_TO_CONNECT, pgStream.ReceiveString(encoding));
				default:
					throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
			}
		}
		while (beresp == 'N');
		// "pg_encoding_to_char(1)" will return 'EUC_JP' for a backend compiled with multibyte,
		// otherwise it's hardcoded to 'SQL_ASCII'.
		// If the backend doesn't know about multibyte we can't assume anything about the encoding
		// used, so we denote this with 'UNKNOWN'.
		//Note: begining with 7.2 we should be using pg_client_encoding() which
		//is new in 7.2.  However it isn't easy to conditionally call this new
		//function, since we don't yet have the information as to what server
		//version we are talking to.  Thus we will continue to call
		//getdatabaseencoding() until we drop support for 7.1 and older versions
		//or until someone comes up with a conditional way to run one or
		//the other function depending on server version that doesn't require
		//two round trips to the server per connection

		final String encodingQuery =
			"case when pg_encoding_to_char(1) = 'SQL_ASCII' then 'UNKNOWN' else getdatabaseencoding() end";

		// Set datestyle and fetch db encoding in a single call, to avoid making
		// more than one round trip to the backend during connection startup.


		BaseResultSet resultSet
			= execSQL("set datestyle to 'ISO'; select version(), " + encodingQuery + ";");
		
		if (! resultSet.next())
		{
			throw new PSQLException("postgresql.con.failed.bad.encoding", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
		}
		String version = resultSet.getString(1);
		dbVersionNumber = extractVersionNumber(version);

		String dbEncoding = resultSet.getString(2);
		encoding = Encoding.getEncoding(dbEncoding, info.getProperty("charSet"));
		
		//TODO: remove this once the set is done as part of V3protocol connection initiation
		if (haveMinimumServerVersion("7.4")) 
		{
			BaseResultSet acRset =
				execSQL("set client_encoding = 'UNICODE'");

			//set encoding to be unicode
			encoding = Encoding.getEncoding("UNICODE", null);
		}

		//In 7.3 we are forced to do a second roundtrip to handle the case 
		//where a database may not be running in autocommit mode
		//jdbc by default assumes autocommit is on until setAutoCommit(false)
		//is called.  Therefore we need to ensure a new connection is 
		//initialized to autocommit on.
		//We also set the client encoding so that the driver only needs 
		//to deal with utf8.  We can only do this in 7.3+ because multibyte 
		//support is now always included
		if (haveMinimumServerVersion("7.3")  && !haveMinimumServerVersion("7.4")) 
		{
			BaseResultSet acRset =
				execSQL("set client_encoding = 'UNICODE'; show autocommit");

			//set encoding to be unicode
			encoding = Encoding.getEncoding("UNICODE", null);

			if (!acRset.next())
			{
				throw new PSQLException("postgresql.con.failed.bad.autocommit", PSQLState.CONNECTION_UNABLE_TO_CONNECT);
			}
			//if autocommit is currently off we need to turn it on
			//note that we will be in a transaction because the select above
			//will have initiated the transaction so we need a commit
			//to make the setting permanent
			if (acRset.getString(1).equals("off"))
			{
				execSQL("set autocommit = on; commit;");
			}
		}

		// Initialise object handling
		initObjectTypes();

		// Mark the connection as ok, and cleanup
		PG_STATUS = CONNECTION_OK;
	}

	/*
	 * Return the instance of org.postgresql.Driver
	 * that created this connection
	 */
	public Driver getDriver()
	{
		return this_driver;
	}


	/*
	 * This adds a warning to the warning chain.
	 * @param msg message to add
	 */
	public void addWarning(String msg)
	{
		// Add the warning to the chain
		if (firstWarning != null)
			firstWarning.setNextWarning(new SQLWarning(msg));
		else
			firstWarning = new SQLWarning(msg);

		// Now check for some specific messages

		// This is obsolete in 6.5, but I've left it in here so if we need to use this
		// technique again, we'll know where to place it.
		//
		// This is generated by the SQL "show datestyle"
		//if (msg.startsWith("NOTICE:") && msg.indexOf("DateStyle")>0) {
		//// 13 is the length off "DateStyle is "
		//msg = msg.substring(msg.indexOf("DateStyle is ")+13);
		//
		//for(int i=0;i<dateStyles.length;i+=2)
		//if (msg.startsWith(dateStyles[i]))
		//currentDateStyle=i+1; // this is the index of the format
		//}
	}

	/** Simple query execution.
	 */
	public BaseResultSet execSQL (String s) throws SQLException
	{
		final Object[] nullarr = new Object[0];
		BaseStatement stat = (BaseStatement) createStatement();
		return QueryExecutor.execute(new String[] { s }, 
									 nullarr, 
									 stat);
	}

	/*
	 * In SQL, a result table can be retrieved through a cursor that
	 * is named.  The current row of a result can be updated or deleted
	 * using a positioned update/delete statement that references the
	 * cursor name.
	 *
	 * We support one cursor per connection.
	 *
	 * setCursorName sets the cursor name.
	 *
	 * @param cursor the cursor name
	 * @exception SQLException if a database access error occurs
	 */
	public void setCursorName(String cursor) throws SQLException
	{
		this.cursor = cursor;
	}

	/*
	 * getCursorName gets the cursor name.
	 *
	 * @return the current cursor name
	 * @exception SQLException if a database access error occurs
	 */
	public String getCursorName() throws SQLException
	{
		return cursor;
	}

	/*
	 * We are required to bring back certain information by
	 * the DatabaseMetaData class.	These functions do that.
	 *
	 * Method getURL() brings back the URL (good job we saved it)
	 *
	 * @return the url
	 * @exception SQLException just in case...
	 */
	public String getURL() throws SQLException
	{
		return this_url;
	}

	/*
	 * Method getUserName() brings back the User Name (again, we
	 * saved it)
	 *
	 * @return the user name
	 * @exception SQLException just in case...
	 */
	int lastMessage = 0;
	public String getUserName() throws SQLException
	{
		return PG_USER;
	}

	/*
	 * Get the character encoding to use for this connection.
	 */
	public Encoding getEncoding() throws SQLException
	{
		return encoding;
	}

	/*
	 * This returns the Fastpath API for the current connection.
	 *
	 * <p><b>NOTE:</b> This is not part of JDBC, but allows access to
	 * functions on the org.postgresql backend itself.
	 *
	 * <p>It is primarily used by the LargeObject API
	 *
	 * <p>The best way to use this is as follows:
	 *
	 * <p><pre>
	 * import org.postgresql.fastpath.*;
	 * ...
	 * Fastpath fp = ((org.postgresql.Connection)myconn).getFastpathAPI();
	 * </pre>
	 *
	 * <p>where myconn is an open Connection to org.postgresql.
	 *
	 * @return Fastpath object allowing access to functions on the org.postgresql
	 * backend.
	 * @exception SQLException by Fastpath when initialising for first time
	 */
	public Fastpath getFastpathAPI() throws SQLException
	{
		if (fastpath == null)
			fastpath = new Fastpath(this, pgStream);
		return fastpath;
	}

	// This holds a reference to the Fastpath API if already open
	private Fastpath fastpath = null;

	/*
	 * This returns the LargeObject API for the current connection.
	 *
	 * <p><b>NOTE:</b> This is not part of JDBC, but allows access to
	 * functions on the org.postgresql backend itself.
	 *
	 * <p>The best way to use this is as follows:
	 *
	 * <p><pre>
	 * import org.postgresql.largeobject.*;
	 * ...
	 * LargeObjectManager lo = ((org.postgresql.Connection)myconn).getLargeObjectAPI();
	 * </pre>
	 *
	 * <p>where myconn is an open Connection to org.postgresql.
	 *
	 * @return LargeObject object that implements the API
	 * @exception SQLException by LargeObject when initialising for first time
	 */
	public LargeObjectManager getLargeObjectAPI() throws SQLException
	{
		if (largeobject == null)
			largeobject = new LargeObjectManager(this);
		return largeobject;
	}

	// This holds a reference to the LargeObject API if already open
	private LargeObjectManager largeobject = null;

	/*
	 * This method is used internally to return an object based around
	 * org.postgresql's more unique data types.
	 *
	 * <p>It uses an internal Hashtable to get the handling class. If the
	 * type is not supported, then an instance of org.postgresql.util.PGobject
	 * is returned.
	 *
	 * You can use the getValue() or setValue() methods to handle the returned
	 * object. Custom objects can have their own methods.
	 *
	 * @return PGobject for this type, and set to value
	 * @exception SQLException if value is not correct for this type
	 */
	public Object getObject(String type, String value) throws SQLException
	{
		try
		{
			Object o = objectTypes.get(type);

			// If o is null, then the type is unknown.
			// If o is not null, and it is a String, then its a class name that
			// extends PGobject.
			//
			// This is used to implement the org.postgresql unique types (like lseg,
			// point, etc).
			if (o != null && o instanceof String)
			{
				// 6.3 style extending PG_Object
				PGobject obj = null;
				obj = (PGobject)(Class.forName((String)o).newInstance());
				obj.setType(type);
				obj.setValue(value);
				return (Object)obj;
			}
		}
		catch (SQLException sx)
		{
			// rethrow the exception. Done because we capture any others next
			sx.fillInStackTrace();
			throw sx;
		}
		catch (Exception ex)
		{
			throw new PSQLException("postgresql.con.creobj", PSQLState.CONNECTION_FAILURE, type, ex);
		}

		// should never be reached
		return null;
	}

	/*
	 * This allows client code to add a handler for one of org.postgresql's
	 * more unique data types.
	 *
	 * <p><b>NOTE:</b> This is not part of JDBC, but an extension.
	 *
	 * <p>The best way to use this is as follows:
	 *
	 * <p><pre>
	 * ...
	 * ((org.postgresql.Connection)myconn).addDataType("mytype","my.class.name");
	 * ...
	 * </pre>
	 *
	 * <p>where myconn is an open Connection to org.postgresql.
	 *
	 * <p>The handling class must extend org.postgresql.util.PGobject
	 *
	 * @see org.postgresql.util.PGobject
	 */
	public void addDataType(String type, String name)
	{
		objectTypes.put(type, name);
	}

	// This holds the available types
	private Hashtable objectTypes = new Hashtable();

	// This array contains the types that are supported as standard.
	//
	// The first entry is the types name on the database, the second
	// the full class name of the handling class.
	//
	private static final String defaultObjectTypes[][] = {
				{"box", "org.postgresql.geometric.PGbox"},
				{"circle", "org.postgresql.geometric.PGcircle"},
				{"line", "org.postgresql.geometric.PGline"},
				{"lseg", "org.postgresql.geometric.PGlseg"},
				{"path", "org.postgresql.geometric.PGpath"},
				{"point", "org.postgresql.geometric.PGpoint"},
				{"polygon", "org.postgresql.geometric.PGpolygon"},
				{"money", "org.postgresql.util.PGmoney"}
			};

	// This initialises the objectTypes hashtable
	private void initObjectTypes()
	{
		for (int i = 0;i < defaultObjectTypes.length;i++)
			objectTypes.put(defaultObjectTypes[i][0], defaultObjectTypes[i][1]);
	}

	/*
	 * In some cases, it is desirable to immediately release a Connection's
	 * database and JDBC resources instead of waiting for them to be
	 * automatically released (cant think why off the top of my head)
	 *
	 * <B>Note:</B> A Connection is automatically closed when it is
	 * garbage collected.  Certain fatal errors also result in a closed
	 * connection.
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void close() throws SQLException
	{
		if (getPGProtocolVersionMajor() == 3) {
			closeV3();
		} else {
			closeV2();
		}
	}

	public void closeV3() throws SQLException
	{
		if (pgStream != null)
		{
			try
			{
				pgStream.SendChar('X');
				pgStream.SendInteger(4,4);
				pgStream.flush();
				pgStream.close();
			}
			catch (IOException e)
			{}
			finally
			{
				pgStream = null;
			}
		}
	}

	public void closeV2() throws SQLException
	{
		if (pgStream != null)
		{
			try
			{
				pgStream.SendChar('X');
				pgStream.flush();
				pgStream.close();
			}
			catch (IOException e)
			{}
			finally
			{
				pgStream = null;
			}
		}
	}

	/*
	 * A driver may convert the JDBC sql grammar into its system's
	 * native SQL grammar prior to sending it; nativeSQL returns the
	 * native form of the statement that the driver would have sent.
	 *
	 * @param sql a SQL statement that may contain one or more '?'
	 *	parameter placeholders
	 * @return the native form of this statement
	 * @exception SQLException if a database access error occurs
	 */
	public String nativeSQL(String sql) throws SQLException
	{
		return sql;
	}

	/*
	 * The first warning reported by calls on this Connection is
	 * returned.
	 *
	 * <B>Note:</B> Sebsequent warnings will be changed to this
	 * SQLWarning
	 *
	 * @return the first SQLWarning or null
	 * @exception SQLException if a database access error occurs
	 */
	public SQLWarning getWarnings() throws SQLException
	{
		return firstWarning;
	}

	/*
	 * After this call, getWarnings returns null until a new warning
	 * is reported for this connection.
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void clearWarnings() throws SQLException
	{
		firstWarning = null;
	}


	/*
	 * You can put a connection in read-only mode as a hunt to enable
	 * database optimizations
	 *
	 * <B>Note:</B> setReadOnly cannot be called while in the middle
	 * of a transaction
	 *
	 * @param readOnly - true enables read-only mode; false disables it
	 * @exception SQLException if a database access error occurs
	 */
	public void setReadOnly(boolean readOnly) throws SQLException
	{
		this.readOnly = readOnly;
	}

	/*
	 * Tests to see if the connection is in Read Only Mode.  Note that
	 * we cannot really put the database in read only mode, but we pretend
	 * we can by returning the value of the readOnly flag
	 *
	 * @return true if the connection is read only
	 * @exception SQLException if a database access error occurs
	 */
	public boolean isReadOnly() throws SQLException
	{
		return readOnly;
	}

	/*
	 * If a connection is in auto-commit mode, than all its SQL
	 * statements will be executed and committed as individual
	 * transactions.  Otherwise, its SQL statements are grouped
	 * into transactions that are terminated by either commit()
	 * or rollback().  By default, new connections are in auto-
	 * commit mode.  The commit occurs when the statement completes
	 * or the next execute occurs, whichever comes first.  In the
	 * case of statements returning a ResultSet, the statement
	 * completes when the last row of the ResultSet has been retrieved
	 * or the ResultSet has been closed.  In advanced cases, a single
	 * statement may return multiple results as well as output parameter
	 * values.	Here the commit occurs when all results and output param
	 * values have been retrieved.
	 *
	 * @param autoCommit - true enables auto-commit; false disables it
	 * @exception SQLException if a database access error occurs
	 */
	public void setAutoCommit(boolean autoCommit) throws SQLException
	{
		if (this.autoCommit == autoCommit)
			return ;
		if (autoCommit)
		{
				execSQL("end");				
		}
		else
		{
			if (haveMinimumServerVersion("7.1"))
			{
				execSQL("begin;" + getIsolationLevelSQL());
			}
			else
			{
				execSQL("begin");
				execSQL(getIsolationLevelSQL());
			}
		}
		this.autoCommit = autoCommit;
	}

	/*
	 * gets the current auto-commit state
	 *
	 * @return Current state of the auto-commit mode
	 * @see setAutoCommit
	 */
	public boolean getAutoCommit()
	{
		return this.autoCommit;
	}

	/*
	 * The method commit() makes all changes made since the previous
	 * commit/rollback permanent and releases any database locks currently
	 * held by the Connection.	This method should only be used when
	 * auto-commit has been disabled.  (If autoCommit == true, then we
	 * just return anyhow)
	 *
	 * @exception SQLException if a database access error occurs
	 * @see setAutoCommit
	 */
	public void commit() throws SQLException
	{
		if (autoCommit)
			return ;
		//TODO: delay starting new transaction until first command
		if (haveMinimumServerVersion("7.1"))
		{
			execSQL("commit;begin;" + getIsolationLevelSQL());
		}
		else
		{
			execSQL("commit");
			execSQL("begin");
			execSQL(getIsolationLevelSQL());
		}
	}

	/*
	 * The method rollback() drops all changes made since the previous
	 * commit/rollback and releases any database locks currently held by
	 * the Connection.
	 *
	 * @exception SQLException if a database access error occurs
	 * @see commit
	 */
	public void rollback() throws SQLException
	{
		if (autoCommit)
			return ;
		//TODO: delay starting transaction until first command
		if (haveMinimumServerVersion("7.1"))
		{
			execSQL("rollback; begin;" + getIsolationLevelSQL());
		}
		else
		{
			execSQL("rollback");
			execSQL("begin");
			execSQL(getIsolationLevelSQL());
		}
	}

	/*
	 * Get this Connection's current transaction isolation mode.
	 *
	 * @return the current TRANSACTION_* mode value
	 * @exception SQLException if a database access error occurs
	 */
	public int getTransactionIsolation() throws SQLException
	{
		String sql = "show transaction isolation level";
		String level = null;
		if (haveMinimumServerVersion("7.3")) {
			BaseResultSet rs = execSQL(sql);
			if (rs.next()) {
				level = rs.getString(1);
			}
			rs.close();
		} else {
			BaseResultSet l_rs = execSQL(sql);
			BaseStatement l_stat = l_rs.getPGStatement();
			SQLWarning warning = l_stat.getWarnings();
			if (warning != null)
			{
				level = warning.getMessage();
			}
			l_rs.close();
			l_stat.close();
		}
		if (level != null) {
			level = level.toUpperCase();
			if (level.indexOf("READ COMMITTED") != -1)
				return Connection.TRANSACTION_READ_COMMITTED;
			else if (level.indexOf("READ UNCOMMITTED") != -1)
				return Connection.TRANSACTION_READ_UNCOMMITTED;
			else if (level.indexOf("REPEATABLE READ") != -1)
				return Connection.TRANSACTION_REPEATABLE_READ;
			else if (level.indexOf("SERIALIZABLE") != -1)
				return Connection.TRANSACTION_SERIALIZABLE;
		}
		return Connection.TRANSACTION_READ_COMMITTED;
	}

	/*
	 * You can call this method to try to change the transaction
	 * isolation level using one of the TRANSACTION_* values.
	 *
	 * <B>Note:</B> setTransactionIsolation cannot be called while
	 * in the middle of a transaction
	 *
	 * @param level one of the TRANSACTION_* isolation values with
	 *	the exception of TRANSACTION_NONE; some databases may
	 *	not support other values
	 * @exception SQLException if a database access error occurs
	 * @see java.sql.DatabaseMetaData#supportsTransactionIsolationLevel
	 */
	public void setTransactionIsolation(int level) throws SQLException
	{
		//In 7.1 and later versions of the server it is possible using
		//the "set session" command to set this once for all future txns
		//however in 7.0 and prior versions it is necessary to set it in
		//each transaction, thus adding complexity below.
		//When we decide to drop support for servers older than 7.1
		//this can be simplified
		isolationLevel = level;
		String isolationLevelSQL;

		if (!haveMinimumServerVersion("7.1"))
		{
			isolationLevelSQL = getIsolationLevelSQL();
		}
		else
		{
			isolationLevelSQL = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ";
			switch (isolationLevel)
			{
				case Connection.TRANSACTION_READ_COMMITTED:
					isolationLevelSQL += "READ COMMITTED";
					break;
				case Connection.TRANSACTION_SERIALIZABLE:
					isolationLevelSQL += "SERIALIZABLE";
					break;
				default:
					throw new PSQLException("postgresql.con.isolevel", PSQLState.TRANSACTION_STATE_INVALID,
											new Integer(isolationLevel));
			}
		}
		execSQL(isolationLevelSQL);
	}

	/*
	 * Helper method used by setTransactionIsolation(), commit(), rollback()
	 * and setAutoCommit(). This returns the SQL string needed to
	 * set the isolation level for a transaction.  In 7.1 and later it
	 * is possible to set a default isolation level that applies to all
	 * future transactions, this method is only necesary for 7.0 and older
	 * servers, and should be removed when support for these older
	 * servers are dropped
	 */
	protected String getIsolationLevelSQL() throws SQLException
	{
		//7.1 and higher servers have a default specified so
		//no additional SQL is required to set the isolation level
		if (haveMinimumServerVersion("7.1"))
		{
			return "";
		}
		StringBuffer sb = new StringBuffer("SET TRANSACTION ISOLATION LEVEL");

		switch (isolationLevel)
		{
			case Connection.TRANSACTION_READ_COMMITTED:
				sb.append(" READ COMMITTED");
				break;

			case Connection.TRANSACTION_SERIALIZABLE:
				sb.append(" SERIALIZABLE");
				break;

			default:
				throw new PSQLException("postgresql.con.isolevel", PSQLState.TRANSACTION_STATE_INVALID, new Integer(isolationLevel));
		}
		return sb.toString();
	}

	/*
	 * A sub-space of this Connection's database may be selected by
	 * setting a catalog name.	If the driver does not support catalogs,
	 * it will silently ignore this request
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void setCatalog(String catalog) throws SQLException
	{
		//no-op
	}

	/*
	 * Return the connections current catalog name, or null if no
	 * catalog name is set, or we dont support catalogs.
	 *
	 * @return the current catalog name or null
	 * @exception SQLException if a database access error occurs
	 */
	public String getCatalog() throws SQLException
	{
		return PG_DATABASE;
	}

	/*
	 * Overides finalize(). If called, it closes the connection.
	 *
	 * This was done at the request of Rachel Greenham
	 * <rachel@enlarion.demon.co.uk> who hit a problem where multiple
	 * clients didn't close the connection, and once a fortnight enough
	 * clients were open to kill the org.postgres server.
	 */
	public void finalize() throws Throwable
	{
		close();
	}

	private static String extractVersionNumber(String fullVersionString)
	{
		StringTokenizer versionParts = new StringTokenizer(fullVersionString);
		versionParts.nextToken(); /* "PostgreSQL" */
		return versionParts.nextToken(); /* "X.Y.Z" */
	}

	/*
	 * Get server version number
	 */
	public String getDBVersionNumber()
	{
		return dbVersionNumber;
	}

	// Parse a "dirty" integer surrounded by non-numeric characters
	private static int integerPart(String dirtyString)
	{
		int start, end;

		for (start = 0; start < dirtyString.length() && !Character.isDigit(dirtyString.charAt(start)); ++start)
			;
		
		for (end = start; end < dirtyString.length() && Character.isDigit(dirtyString.charAt(end)); ++end)
			;

		if (start == end)
			return 0;

		return Integer.parseInt(dirtyString.substring(start, end));
	}

	/*
	 * Get server major version
	 */
	public int getServerMajorVersion()
	{        
		try
		{
			StringTokenizer versionTokens = new StringTokenizer(dbVersionNumber, ".");  // aaXbb.ccYdd       
			return integerPart(versionTokens.nextToken()); // return X
		}
		catch (NoSuchElementException e)
		{
			return 0;
		}
	}

	/*
	 * Get server minor version
	 */
	public int getServerMinorVersion()
	{        
		try
		{
			StringTokenizer versionTokens = new StringTokenizer(dbVersionNumber, ".");  // aaXbb.ccYdd
			versionTokens.nextToken(); // Skip aaXbb
			return integerPart(versionTokens.nextToken()); // return Y
		}
		catch (NoSuchElementException e)
		{
			return 0;
		}
	}
	
	/**
	 * Is the server we are connected to running at least this version?
	 * This comparison method will fail whenever a major or minor version
	 * goes to two digits (10.3.0) or (7.10.1).
	 */
	public boolean haveMinimumServerVersion(String ver) throws SQLException
	{
		return (getDBVersionNumber().compareTo(ver) >= 0);
	}

	/*
	 * This method returns true if the compatible level set in the connection
	 * (which can be passed into the connection or specified in the URL)
	 * is at least the value passed to this method.  This is used to toggle
	 * between different functionality as it changes across different releases
	 * of the jdbc driver code.  The values here are versions of the jdbc client
	 * and not server versions.  For example in 7.1 get/setBytes worked on
	 * LargeObject values, in 7.2 these methods were changed to work on bytea
	 * values.	This change in functionality could be disabled by setting the
	 * "compatible" level to be 7.1, in which case the driver will revert to
	 * the 7.1 functionality.
	 */
	public boolean haveMinimumCompatibleVersion(String ver) throws SQLException
	{
		return (compatible.compareTo(ver) >= 0);
	}


	/*
	 * This returns the java.sql.Types type for a PG type oid
	 *
	 * @param oid PostgreSQL type oid
	 * @return the java.sql.Types type
	 * @exception SQLException if a database access error occurs
	 */
	public int getSQLType(int oid) throws SQLException
	{
		Integer sqlType = (Integer)sqlTypeCache.get(new Integer(oid));

		// it's not in the cache, so perform a query, and add the result to the cache
		if (sqlType == null)
		{
			String pgType;
			// The opaque type does not exist in the system catalogs.
			if (oid == 0) {
				pgType = "opaque";
			} else {
				String sql;
				if (haveMinimumServerVersion("7.3")) {
					sql = "SELECT typname FROM pg_catalog.pg_type WHERE oid = " +oid;
				} else {
					sql = "SELECT typname FROM pg_type WHERE oid = " +oid;
				}
				BaseResultSet result = execSQL(sql);
				if (result.getColumnCount() != 1 || result.getTupleCount() != 1) {
					throw new PSQLException("postgresql.unexpected", PSQLState.UNEXPECTED_ERROR);
				}
				result.next();
				pgType = result.getString(1);
				result.close();
			}
			Integer iOid = new Integer(oid);
			sqlType = new Integer(getSQLType(pgType));
			sqlTypeCache.put(iOid, sqlType);
			pgTypeCache.put(iOid, pgType);
		}

		return sqlType.intValue();
	}

	/*
	 * This returns the oid for a given PG data type
	 * @param typeName PostgreSQL type name
	 * @return PostgreSQL oid value for a field of this type
	 */
	public int getPGType(String typeName) throws SQLException
	{
		int oid = -1;
		if (typeName != null)
		{
			Integer oidValue = (Integer) typeOidCache.get(typeName);
			if (oidValue != null)
			{
				oid = oidValue.intValue();
			}
			else
			{
				// it's not in the cache, so perform a query, and add the result to the cache
				String sql;
				if (haveMinimumServerVersion("7.3")) {
					sql = "SELECT oid FROM pg_catalog.pg_type WHERE typname='" + typeName + "'";
				} else {
					sql = "SELECT oid FROM pg_type WHERE typname='" + typeName + "'";
				}
				BaseResultSet result = execSQL(sql);
				if (result.getColumnCount() != 1 || result.getTupleCount() != 1)
					throw new PSQLException("postgresql.unexpected", PSQLState.UNEXPECTED_ERROR);
				result.next();
				oid = Integer.parseInt(result.getString(1));
				typeOidCache.put(typeName, new Integer(oid));
				result.close();
			}
		}
		return oid;
	}

	/*
	 * We also need to get the PG type name as returned by the back end.
	 *
	 * @return the String representation of the type of this field
	 * @exception SQLException if a database access error occurs
	 */
	public String getPGType(int oid) throws SQLException
	{
		String pgType = (String) pgTypeCache.get(new Integer(oid));
		if (pgType == null)
		{
			getSQLType(oid);
			pgType = (String) pgTypeCache.get(new Integer(oid));
		}
		return pgType;
	}

	//Because the get/setLogStream methods are deprecated in JDBC2
	//we use them for JDBC1 here and override this method in the jdbc2
	//version of this class
	protected void enableDriverManagerLogging()
	{
		if (DriverManager.getLogStream() == null)
		{
			DriverManager.setLogStream(System.out);
		}
	}

	// This is a cache of the DatabaseMetaData instance for this connection
	protected java.sql.DatabaseMetaData metadata;


	/*
	 * Tests to see if a Connection is closed
	 *
	 * @return the status of the connection
	 * @exception SQLException (why?)
	 */
	public boolean isClosed() throws SQLException
	{
		return (pgStream == null);
	}

	/*
	 * This implemetation uses the jdbc1Types array to support the jdbc1
	 * datatypes.  Basically jdbc1 and jdbc2 are the same, except that
	 * jdbc2 adds the Array types.
	 */
	public int getSQLType(String pgTypeName)
	{
		int sqlType = Types.OTHER; // default value
		for (int i = 0;i < jdbc1Types.length;i++)
		{
			if (pgTypeName.equals(jdbc1Types[i]))
			{
				sqlType = jdbc1Typei[i];
				break;
			}
		}
		return sqlType;
	}

	/*
	 * This table holds the org.postgresql names for the types supported.
	 * Any types that map to Types.OTHER (eg POINT) don't go into this table.
	 * They default automatically to Types.OTHER
	 *
	 * Note: This must be in the same order as below.
	 *
	 * Tip: keep these grouped together by the Types. value
	 */
	private static final String jdbc1Types[] = {
				"int2",
				"int4", "oid",
				"int8",
				"cash", "money",
				"numeric",
				"float4",
				"float8",
				"bpchar", "char", "char2", "char4", "char8", "char16",
				"varchar", "text", "name", "filename",
				"bytea",
				"bool",
				"bit",
				"date",
				"time",
				"abstime", "timestamp", "timestamptz"
			};

	/*
	 * This table holds the JDBC type for each entry above.
	 *
	 * Note: This must be in the same order as above
	 *
	 * Tip: keep these grouped together by the Types. value
	 */
	private static final int jdbc1Typei[] = {
		Types.SMALLINT,
		Types.INTEGER, Types.INTEGER,
		Types.BIGINT,
		Types.DOUBLE, Types.DOUBLE,
		Types.NUMERIC,
		Types.REAL,
		Types.DOUBLE,
		Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR,
		Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
		Types.BINARY,
		Types.BIT,
		Types.BIT,
		Types.DATE,
		Types.TIME,
		Types.TIMESTAMP, Types.TIMESTAMP, Types.TIMESTAMP
	};

	public void cancelQuery() throws SQLException
	{
		org.postgresql.core.PGStream cancelStream = null;
		try
		{
			cancelStream = new org.postgresql.core.PGStream(PG_HOST, PG_PORT);
		}
		catch (ConnectException cex)
		{
			// Added by Peter Mount <peter@retep.org.uk>
			// ConnectException is thrown when the connection cannot be made.
			// we trap this an return a more meaningful message for the end user
			throw new PSQLException ("postgresql.con.refused", PSQLState.CONNECTION_REJECTED);
		}
		catch (IOException e)
		{
			throw new PSQLException ("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}

		// Now we need to construct and send a cancel packet
		try
		{
			cancelStream.SendInteger(16, 4);
			cancelStream.SendInteger(80877102, 4);
			cancelStream.SendInteger(pid, 4);
			cancelStream.SendInteger(ckey, 4);
			cancelStream.flush();
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.failed", PSQLState.CONNECTION_UNABLE_TO_CONNECT, e);
		}
		finally
		{
			try
			{
				if (cancelStream != null)
					cancelStream.close();
			}
			catch (IOException e)
			{} // Ignore
		}
	}


	//Methods to support postgres notifications
	public void addNotification(org.postgresql.PGNotification p_notification)
	{
		if (m_notifications == null)
			m_notifications = new Vector();
		m_notifications.addElement(p_notification);
	}

	public PGNotification[] getNotifications()
	{
		PGNotification[] l_return = null;
		if (m_notifications != null)
		{
			l_return = new PGNotification[m_notifications.size()];
			m_notifications.copyInto(l_return);
		}
		m_notifications = null;
		return l_return;
	}
}


