
package org.postgresql.core;

import java.util.Vector;
import java.io.IOException;
import java.sql.*;
import org.postgresql.*;
import org.postgresql.util.PSQLException;

/*
 * Executes a query on the backend.
 *
 * <p>The lifetime of a QueryExecutor object is from sending the query
 * until the response has been received from the backend.
 *
 * $Id: QueryExecutor.java,v 1.7 2002/03/05 18:01:27 davec Exp $
 */

public class QueryExecutor
{

	private final String sql;
	private final java.sql.Statement statement;
	private final PG_Stream pg_stream;
	private final org.postgresql.Connection connection;

	public QueryExecutor(String sql,
						 java.sql.Statement statement,
						 PG_Stream pg_stream,
						 org.postgresql.Connection connection)
	throws SQLException
	{
		this.sql = sql;
		this.statement = statement;
		this.pg_stream = pg_stream;
		this.connection = connection;

		if (statement != null)
			maxRows = statement.getMaxRows();
		else
			maxRows = 0;
	}

	private Field[] fields = null;
	private Vector tuples = new Vector();
	private boolean binaryCursor = false;
	private String status = null;
	private int update_count = 1;
	private long insert_oid = 0;
	private int maxRows;

	/*
	 * Execute a query on the backend.
	 */
	public java.sql.ResultSet execute() throws SQLException
	{

		int fqp = 0;
		boolean hfr = false;
		int lastMessage = 0;

		synchronized (pg_stream)
		{

			sendQuery(sql);

			while (!hfr || fqp > 0)
			{
				int c = pg_stream.ReceiveChar();

				switch (c)
				{
					case 'A':	// Asynchronous Notify
						int pid = pg_stream.ReceiveInteger(4);
						String msg = pg_stream.ReceiveString(connection.getEncoding());
						break;
					case 'B':	// Binary Data Transfer
						receiveTuple(true);
						break;
					case 'C':	// Command Status
						receiveCommandStatus();

						if (fields != null)
							hfr = true;
						else
						{
							sendQuery(" ");
							fqp++;
						}
						break;
					case 'D':	// Text Data Transfer
						receiveTuple(false);
						break;
					case 'E':	// Error Message
						throw new SQLException(pg_stream.ReceiveString(connection.getEncoding()));
					case 'I':	// Empty Query
						int t = pg_stream.ReceiveChar();
						if (t != 0)
							throw new PSQLException("postgresql.con.garbled");

						if (fqp > 0)
							fqp--;
						if (fqp == 0)
							hfr = true;
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
					case 'Z':		 // backend ready for query, ignore for now :-)
						if ( lastMessage == 'Z' )
 			                        {
						     try
                        			     {
                                    			pg_stream.SendChar('Q');
                                    			pg_stream.SendChar(' ');
                                    			pg_stream.SendChar(0);
                                    			pg_stream.flush();
                                 		     } catch (IOException e) {
                                    			throw new PSQLException("postgresql.con.ioerror",e);
                                 		     }
                              			     fqp++;
                           			}
 					
						break;
					default:
						throw new PSQLException("postgresql.con.type",
												new Character((char) c));
				}
				lastMessage = c;
			}
			return connection.getResultSet(connection, statement, fields, tuples, status, update_count, insert_oid, binaryCursor);
		}
	}

	/*
	 * Send a query to the backend.
	 */
	private void sendQuery(String query) throws SQLException
	{
		try
		{
			pg_stream.SendChar('Q');
			pg_stream.Send(connection.getEncoding().encode(query));
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
