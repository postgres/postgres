
package org.postgresql.core;

import java.util.Vector;
import java.io.IOException;
import java.sql.*;
import org.postgresql.*;
import org.postgresql.util.PSQLException;
import org.postgresql.jdbc1.AbstractJdbc1Connection;
import org.postgresql.jdbc1.AbstractJdbc1ResultSet;
import org.postgresql.jdbc1.AbstractJdbc1Statement;

/*
 * Executes a query on the backend.
 *
 * <p>The lifetime of a QueryExecutor object is from sending the query
 * until the response has been received from the backend.
 *
 * $Id: QueryExecutor.java,v 1.19 2003/02/04 11:01:52 davec Exp $
 */

public class QueryExecutor
{
	//This version of execute does not take an existing result set, but 
    //creates a new one for the results of the query
	public static ResultSet execute (String[] p_sqlFrags,
				    Object[] p_binds,
				    java.sql.Statement statement)
	throws SQLException
	{
		QueryExecutor qe = new QueryExecutor();
		qe.m_sqlFrags = p_sqlFrags;
		qe.m_binds = p_binds;
		qe.statement = statement;
		if (statement != null)
			qe.maxRows = statement.getMaxRows();
		else
			qe.maxRows = 0;

        qe.connection = (AbstractJdbc1Connection)((AbstractJdbc1Statement)statement).getPGConnection();
		qe.pg_stream = 	qe.connection.getPGStream();

		return qe.execute();
	}

	//This version of execute reuses an existing result set for the query 
    //results, this is used when a result set is backed by a cursor and 
	//more results are fetched
	public static void execute (String[] p_sqlFrags,
				    Object[] p_binds,
				    java.sql.ResultSet rs)
	throws SQLException
	{
		QueryExecutor qe = new QueryExecutor();
		qe.m_sqlFrags = p_sqlFrags;
		qe.m_binds = p_binds;
		qe.rs = rs;
		qe.statement = (java.sql.Statement)((AbstractJdbc1ResultSet)qe.rs).getPGStatement();
		if (qe.statement != null)
			qe.maxRows = qe.statement.getMaxRows();
		else
			qe.maxRows = 0;

        qe.connection = (AbstractJdbc1Connection)((AbstractJdbc1Statement)qe.statement).getPGConnection();
		qe.pg_stream = 	qe.connection.getPGStream();

		qe.execute();
	}


	private QueryExecutor ()
	{
	}

   	private String[] m_sqlFrags;
	private Object[] m_binds;
	private java.sql.Statement statement;
	private java.sql.ResultSet rs;

   	private AbstractJdbc1Connection connection;
   	private PG_Stream pg_stream;

	private Field[] fields = null;
	private Vector tuples = new Vector();
	private boolean binaryCursor = false;
	private String status = null;
	private int update_count = 1;
	private long insert_oid = 0;
	private int maxRows;


	/*
	 * Execute a query on the backend.
	 *
	 */
	private java.sql.ResultSet execute() throws SQLException
	{

		StringBuffer errorMessage = null;

		if (pg_stream == null) 
		{
			throw new PSQLException("postgresql.con.closed");
		}

		synchronized (pg_stream)
		{

			sendQuery();

			int c;
			boolean l_endQuery = false;
			while (!l_endQuery)
			{
				c = pg_stream.ReceiveChar();

				switch (c)
				{
					case 'A':	// Asynchronous Notify
						int pid = pg_stream.ReceiveInteger(4);
						String msg = pg_stream.ReceiveString(connection.getEncoding());
						connection.addNotification(new org.postgresql.core.Notification(msg, pid));
						break;
					case 'B':	// Binary Data Transfer
						receiveTuple(true);
						break;
					case 'C':	// Command Status
						receiveCommandStatus();
						break;
					case 'D':	// Text Data Transfer
						receiveTuple(false);
						break;
					case 'E':	// Error Message

						// it's possible to get more than one error message for a query
						// see libpq comments wrt backend closing a connection
						// so, append messages to a string buffer and keep processing
						// check at the bottom to see if we need to throw an exception

						if ( errorMessage == null )
							errorMessage = new StringBuffer();

						errorMessage.append(pg_stream.ReceiveString(connection.getEncoding()));
						// keep processing
						break;
					case 'I':	// Empty Query
						int t = pg_stream.ReceiveChar();
						break;
					case 'N':	// Error Notification
						connection.addWarning(pg_stream.ReceiveString(connection.getEncoding()));
						break;
					case 'P':	// Portal Name
						String pname = pg_stream.ReceiveString(connection.getEncoding());
						break;
					case 'T':	// MetaData Field Description
						receiveFields();
						break;
					case 'Z':
						l_endQuery = true;
						break;
					default:
						throw new PSQLException("postgresql.con.type",
												new Character((char) c));
				}

			}

			// did we get an error during this query?
			if ( errorMessage != null )
				throw new SQLException( errorMessage.toString() );


			//if an existing result set was passed in reuse it, else
			//create a new one
			if (rs != null) 
			{
				((org.postgresql.jdbc1.AbstractJdbc1ResultSet)rs).reInit(fields, tuples, status, update_count, insert_oid, binaryCursor);
			}
			else 
			{
				rs = ((AbstractJdbc1Statement)statement).createResultSet(fields, tuples, status, update_count, insert_oid, binaryCursor);
			}
			return rs;
		}
	}

	/*
	 * Send a query to the backend.
	 */
	private void sendQuery() throws SQLException
	{
		for ( int i = 0; i < m_binds.length ; i++ )
		{
			if ( m_binds[i] == null )
				throw new PSQLException("postgresql.prep.param", new Integer(i + 1));
		}
		try
		{
			pg_stream.SendChar('Q');
			for (int i = 0 ; i < m_binds.length ; ++i)
			{
				pg_stream.Send(connection.getEncoding().encode(m_sqlFrags[i]));
				pg_stream.Send(connection.getEncoding().encode(m_binds[i].toString()));
			}

			pg_stream.Send(connection.getEncoding().encode(m_sqlFrags[m_binds.length]));
			pg_stream.SendChar(0);
			pg_stream.flush();

		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.ioerror", e);
		}
	}

	/*
	 * Receive a tuple from the backend.
	 *
	 * @param isBinary set if the tuple should be treated as binary data
	 */
	private void receiveTuple(boolean isBinary) throws SQLException
	{
		if (fields == null)
			throw new PSQLException("postgresql.con.tuple");
		Object tuple = pg_stream.ReceiveTuple(fields.length, isBinary);
		if (isBinary)
			binaryCursor = true;
		if (maxRows == 0 || tuples.size() < maxRows)
			tuples.addElement(tuple);
	}

	/*
	 * Receive command status from the backend.
	 */
	private void receiveCommandStatus() throws SQLException
	{

		status = pg_stream.ReceiveString(connection.getEncoding());

		try
		{
			// Now handle the update count correctly.
			if (status.startsWith("INSERT") || status.startsWith("UPDATE") || status.startsWith("DELETE") || status.startsWith("MOVE"))
			{
				update_count = Integer.parseInt(status.substring(1 + status.lastIndexOf(' ')));
			}
			if (status.startsWith("INSERT"))
			{
				insert_oid = Long.parseLong(status.substring(1 + status.indexOf(' '),
											status.lastIndexOf(' ')));
			}
		}
		catch (NumberFormatException nfe)
		{
			throw new PSQLException("postgresql.con.fathom", status);
		}
	}

	/*
	 * Receive the field descriptions from the back end.
	 */
	private void receiveFields() throws SQLException
	{
		if (fields != null)
			throw new PSQLException("postgresql.con.multres");

		int size = pg_stream.ReceiveIntegerR(2);
		fields = new Field[size];

		for (int i = 0; i < fields.length; i++)
		{
			String typeName = pg_stream.ReceiveString(connection.getEncoding());
			int typeOid = pg_stream.ReceiveIntegerR(4);
			int typeLength = pg_stream.ReceiveIntegerR(2);
			int typeModifier = pg_stream.ReceiveIntegerR(4);
			fields[i] = new Field(connection, typeName, typeOid, typeLength, typeModifier);
		}
	}
}
