/*-------------------------------------------------------------------------
 *
 * Fastpath.java
 *     This class implements the Fastpath api.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/fastpath/Attic/Fastpath.java,v 1.16.2.4 2004/08/11 06:56:00 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.fastpath;

import java.io.IOException;
import java.sql.SQLException;
import java.sql.ResultSet;
import java.util.Hashtable;
import org.postgresql.Driver;
import org.postgresql.core.BaseConnection;
import org.postgresql.core.PGStream;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

/*
 * This class implements the Fastpath api.
 *
 * <p>This is a means of executing functions imbeded in the org.postgresql
 *  backend from within a java application.
 *
 * <p>It is based around the file src/interfaces/libpq/fe-exec.c
 *
 */
public class Fastpath
{
	// This maps the functions names to their id's (possible unique just
	// to a connection).
	protected Hashtable func = new Hashtable();

	protected BaseConnection conn;		// our connection
	protected PGStream stream;	// the network stream

	/*
	 * Initialises the fastpath system
	 *
	 * @param conn BaseConnection to attach to
	 * @param stream The network stream to the backend
	 */
	public Fastpath(BaseConnection conn, PGStream stream)
	{
		this.conn = conn;
		this.stream = stream;
	}

	/*
	 * Send a function call to the PostgreSQL backend
	 *
	 * @param fnid Function id
	 * @param resulttype True if the result is an integer, false for other results
	 * @param args FastpathArguments to pass to fastpath
	 * @return null if no data, Integer if an integer result, or byte[] otherwise
	 * @exception SQLException if a database-access error occurs.
	 */
	public Object fastpath(int fnid, boolean resulttype, FastpathArg[] args) throws SQLException
	{
		if (conn.getPGProtocolVersionMajor() == 3) {
			return fastpathV3(fnid, resulttype, args);
		} else {
			return fastpathV2(fnid, resulttype, args);
		}
	}

	private Object fastpathV3(int fnid, boolean resulttype, FastpathArg[] args) throws SQLException
	{
		// added Oct 7 1998 to give us thread safety
		synchronized (stream)
		{
			// send the function call
			try
			{
				int l_msgLen = 0;
				l_msgLen += 16;
				for (int i=0;i < args.length;i++)
					l_msgLen += args[i].sendSize();
					
				stream.SendChar('F');
				stream.SendInteger(l_msgLen,4);
				stream.SendInteger(fnid, 4);
				stream.SendInteger(1,2);
				stream.SendInteger(1,2);
				stream.SendInteger(args.length,2);

				for (int i = 0;i < args.length;i++)
					args[i].send(stream);
				
				stream.SendInteger(1,2);
	
				// This is needed, otherwise data can be lost
				stream.flush();

			}
			catch (IOException ioe)
			{
				throw new PSQLException("postgresql.fp.send", PSQLState.COMMUNICATION_ERROR, new Integer(fnid), ioe);
			}

			// Now handle the result

			// Now loop, reading the results
			Object result = null; // our result
			PSQLException error = null;
			int c;
			boolean l_endQuery = false;
			while (!l_endQuery)
			{
				c = stream.ReceiveChar();

				switch (c)
				{
					case 'A':	// Asynchronous Notify
						int msglen = stream.ReceiveIntegerR(4);
						int pid = stream.ReceiveIntegerR(4);
						String msg = stream.ReceiveString(conn.getEncoding());
						String param = stream.ReceiveString(conn.getEncoding());
						conn.addNotification(new org.postgresql.core.Notification(msg, pid));
						break;
						//------------------------------
						// Error message returned
					case 'E':
						int l_elen = stream.ReceiveIntegerR(4);
						String totalMessage = conn.getEncoding().decode(stream.Receive(l_elen-4));
						PSQLException l_error = PSQLException.parseServerError(totalMessage);

						if (error != null) {
							error.setNextException(l_error);
						} else {
							error = l_error;
						}

						break;
						//------------------------------
						// Notice from backend
					case 'N':
						int l_nlen = stream.ReceiveIntegerR(4);
						PSQLException notify = PSQLException.parseServerError(conn.getEncoding().decode(stream.Receive(l_nlen-4)));
						conn.addWarning(notify.getMessage());
						break;

					case 'V':
						int l_msgLen = stream.ReceiveIntegerR(4);
						int l_valueLen = stream.ReceiveIntegerR(4);
						
						if (l_valueLen == -1) 
						{
							//null value
						}
						else if (l_valueLen == 0)
						{
							result = new byte[0];
						}
						else 
						{
							// Return an Integer if
							if (resulttype)
								result = new Integer(stream.ReceiveIntegerR(l_valueLen));
							else
							{
								byte buf[] = new byte[l_valueLen];
								stream.Receive(buf, 0, l_valueLen);
								result = buf;
							}
						}
						break;

					case 'Z':
						//TODO: use size better
						if (stream.ReceiveIntegerR(4) != 5) throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT); 
						//TODO: handle transaction status
						char l_tStatus = (char)stream.ReceiveChar();
						l_endQuery = true;
						break;

					default:
						throw new PSQLException("postgresql.fp.protocol", PSQLState.COMMUNICATION_ERROR, new Character((char)c));
				}
			}

			if ( error != null )
				throw error;

			return result;
		}
	}

	private Object fastpathV2(int fnid, boolean resulttype, FastpathArg[] args) throws SQLException
	{
		// added Oct 7 1998 to give us thread safety
		synchronized (stream)
		{
			// send the function call
			try
			{
				// 70 is 'F' in ASCII. Note: don't use SendChar() here as it adds padding
				// that confuses the backend. The 0 terminates the command line.
				stream.SendInteger(70, 1);
				stream.SendInteger(0, 1);

				stream.SendInteger(fnid, 4);
				stream.SendInteger(args.length, 4);

				for (int i = 0;i < args.length;i++)
					args[i].send(stream);

				// This is needed, otherwise data can be lost
				stream.flush();

			}
			catch (IOException ioe)
			{
				//Should be sending exception as second arg.
				throw new PSQLException("postgresql.fp.send", PSQLState.COMMUNICATION_ERROR, new Integer(fnid), ioe);
			}

			// Now handle the result

			// Now loop, reading the results
			Object result = null; // our result
			StringBuffer errorMessage = null;
			int c;
			boolean l_endQuery = false;
			while (!l_endQuery)
			{
				c = stream.ReceiveChar();

				switch (c)
				{
					case 'A':	// Asynchronous Notify
						//TODO: do something with this
						int pid = stream.ReceiveIntegerR(4);
						String msg = stream.ReceiveString(conn.getEncoding());
						conn.addNotification(new org.postgresql.core.Notification(msg, pid));
						break;

						//------------------------------
						// Error message returned
					case 'E':
						if ( errorMessage == null )
							errorMessage = new StringBuffer();
						errorMessage.append(stream.ReceiveString(conn.getEncoding()));
						break;

						//------------------------------
						// Notice from backend
					case 'N':
						conn.addWarning(stream.ReceiveString(conn.getEncoding()));
						break;

					case 'V':
						int l_nextChar = stream.ReceiveChar();
						if (l_nextChar == 'G')
						{
							int sz = stream.ReceiveIntegerR(4);
							// Return an Integer if
							if (resulttype)
								result = new Integer(stream.ReceiveIntegerR(sz));
							else
							{
								byte buf[] = new byte[sz];
								stream.Receive(buf, 0, sz);
								result = buf;
							}
							//There should be a trailing '0'
							int l_endChar = stream.ReceiveChar();
						}
						else
						{
							//it must have been a '0', thus no results
						}
						break;

					case 'Z':
						l_endQuery = true;
						break;

					default:
						throw new PSQLException("postgresql.fp.protocol", PSQLState.COMMUNICATION_ERROR, new Character((char)c));
				}
			}

			if ( errorMessage != null )
				throw new PSQLException("postgresql.fp.error", PSQLState.COMMUNICATION_ERROR, errorMessage.toString());

			return result;
		}
	}

	/*
	 * Send a function call to the PostgreSQL backend by name.
	 *
	 * Note: the mapping for the procedure name to function id needs to exist,
	 * usually to an earlier call to addfunction().
	 *
	 * This is the prefered method to call, as function id's can/may change
	 * between versions of the backend.
	 *
	 * For an example of how this works, refer to org.postgresql.largeobject.LargeObject
	 *
	 * @param name Function name
	 * @param resulttype True if the result is an integer, false for other
	 * results
	 * @param args FastpathArguments to pass to fastpath
	 * @return null if no data, Integer if an integer result, or byte[] otherwise
	 * @exception SQLException if name is unknown or if a database-access error
	 * occurs.
	 * @see org.postgresql.largeobject.LargeObject
	 */
	public Object fastpath(String name, boolean resulttype, FastpathArg[] args) throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("Fastpath: calling " + name);
		return fastpath(getID(name), resulttype, args);
	}

	/*
	 * This convenience method assumes that the return value is an Integer
	 * @param name Function name
	 * @param args Function arguments
	 * @return integer result
	 * @exception SQLException if a database-access error occurs or no result
	 */
	public int getInteger(String name, FastpathArg[] args) throws SQLException
	{
		Integer i = (Integer)fastpath(name, true, args);
		if (i == null)
			throw new PSQLException("postgresql.fp.expint", name);
		return i.intValue();
	}

	/*
	 * This convenience method assumes that the return value is an Integer
	 * @param name Function name
	 * @param args Function arguments
	 * @return byte[] array containing result
	 * @exception SQLException if a database-access error occurs or no result
	 */
	public byte[] getData(String name, FastpathArg[] args) throws SQLException
	{
		return (byte[])fastpath(name, false, args);
	}

	/*
	 * This adds a function to our lookup table.
	 *
	 * <p>User code should use the addFunctions method, which is based upon a
	 * query, rather than hard coding the oid. The oid for a function is not
	 * guaranteed to remain static, even on different servers of the same
	 * version.
	 *
	 * @param name Function name
	 * @param fnid Function id
	 */
	public void addFunction(String name, int fnid)
	{
		func.put(name, new Integer(fnid));
	}

	/*
	 * This takes a ResultSet containing two columns. Column 1 contains the
	 * function name, Column 2 the oid.
	 *
	 * <p>It reads the entire ResultSet, loading the values into the function
	 * table.
	 *
	 * <p><b>REMEMBER</b> to close() the resultset after calling this!!
	 *
	 * <p><b><em>Implementation note about function name lookups:</em></b>
	 *
	 * <p>PostgreSQL stores the function id's and their corresponding names in
	 * the pg_proc table. To speed things up locally, instead of querying each
	 * function from that table when required, a Hashtable is used. Also, only
	 * the function's required are entered into this table, keeping connection
	 * times as fast as possible.
	 *
	 * <p>The org.postgresql.largeobject.LargeObject class performs a query upon it's startup,
	 * and passes the returned ResultSet to the addFunctions() method here.
	 *
	 * <p>Once this has been done, the LargeObject api refers to the functions by
	 * name.
	 *
	 * <p>Dont think that manually converting them to the oid's will work. Ok,
	 * they will for now, but they can change during development (there was some
	 * discussion about this for V7.0), so this is implemented to prevent any
	 * unwarranted headaches in the future.
	 *
	 * @param rs ResultSet
	 * @exception SQLException if a database-access error occurs.
	 * @see org.postgresql.largeobject.LargeObjectManager
	 */
	public void addFunctions(ResultSet rs) throws SQLException
	{
		while (rs.next())
		{
			func.put(rs.getString(1), new Integer(rs.getInt(2)));
		}
	}

	/*
	 * This returns the function id associated by its name
	 *
	 * <p>If addFunction() or addFunctions() have not been called for this name,
	 * then an SQLException is thrown.
	 *
	 * @param name Function name to lookup
	 * @return Function ID for fastpath call
	 * @exception SQLException is function is unknown.
	 */
	public int getID(String name) throws SQLException
	{
		Integer id = (Integer)func.get(name);

		// may be we could add a lookup to the database here, and store the result
		// in our lookup table, throwing the exception if that fails.
		// We must, however, ensure that if we do, any existing ResultSet is
		// unaffected, otherwise we could break user code.
		//
		// so, until we know we can do this (needs testing, on the TODO list)
		// for now, we throw the exception and do no lookups.
		if (id == null)
			throw new PSQLException("postgresql.fp.unknown", PSQLState.UNEXPECTED_ERROR, name);

		return id.intValue();
	}
}

