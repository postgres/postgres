/*-------------------------------------------------------------------------
 *
 * QueryExecutor.java
 *     Executes a query on the backend.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/core/Attic/QueryExecutor.java,v 1.27.2.3 2004/08/11 06:56:00 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.core;

import java.util.Vector;
import java.io.IOException;
import java.sql.*;
import org.postgresql.Driver;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

public class QueryExecutor
{
	//This version of execute does not take an existing result set, but 
    //creates a new one for the results of the query
	public static BaseResultSet execute(String[] p_sqlFrags,
				    Object[] p_binds,
				    BaseStatement statement)
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

        qe.connection = statement.getPGConnection();
		qe.pgStream = qe.connection.getPGStream();

		return qe.execute();
	}

	//This version of execute reuses an existing result set for the query 
    //results, this is used when a result set is backed by a cursor and 
	//more results are fetched
	public static void execute(String[] p_sqlFrags,
				    Object[] p_binds,
				    BaseResultSet rs)
	throws SQLException
	{
		QueryExecutor qe = new QueryExecutor();
		qe.m_sqlFrags = p_sqlFrags;
		qe.m_binds = p_binds;
		qe.rs = rs;
		qe.statement = qe.rs.getPGStatement();
		if (qe.statement != null)
			qe.maxRows = qe.statement.getMaxRows();
		else
			qe.maxRows = 0;

        qe.connection = qe.statement.getPGConnection();
		qe.pgStream = 	qe.connection.getPGStream();

		qe.execute();
	}


	private QueryExecutor ()
	{
	}

   	private String[] m_sqlFrags;
	private Object[] m_binds;
	private BaseStatement statement;
	private BaseResultSet rs;

   	private BaseConnection connection;
   	private PGStream pgStream;

	private Field[] fields = null;
	private Vector tuples = new Vector();
	private String status = null;
	private int update_count = 1;
	private long insert_oid = 0;
	private int maxRows;


	/*
	 * Execute a query on the backend.
	 *
	 */
	private BaseResultSet execute() throws SQLException
	{
		if (connection.getPGProtocolVersionMajor() == 3) {
			if (Driver.logDebug)
				Driver.debug("Using Protocol Version3 to send query");
			return executeV3();
		} else {
			if (Driver.logDebug)
				Driver.debug("Using Protocol Version2 to send query");
			return executeV2();
		}
	}

	private BaseResultSet executeV3() throws SQLException
	{

		PSQLException error = null;

		if (pgStream == null) 
		{
			throw new PSQLException("postgresql.con.closed", PSQLState.CONNECTION_DOES_NOT_EXIST);
		}

		synchronized (pgStream)
		{

			sendQueryV3();

			int c;
			boolean l_endQuery = false;
			while (!l_endQuery)
			{
				c = pgStream.ReceiveChar();
				switch (c)
				{
					case 'A':	// Asynchronous Notify
						int msglen = pgStream.ReceiveIntegerR(4);
						int pid = pgStream.ReceiveIntegerR(4);
						String msg = pgStream.ReceiveString(connection.getEncoding());
						String param = pgStream.ReceiveString(connection.getEncoding());
						connection.addNotification(new org.postgresql.core.Notification(msg, pid));
						break;
					case 'C':	// Command Status
						receiveCommandStatusV3();
						break;
					case 'D':	// Data Transfer
						receiveTupleV3();
						break;
					case 'E':	// Error Message

						// it's possible to get more than one error message for a query
						// see libpq comments wrt backend closing a connection
						// so, append messages to a string buffer and keep processing
						// check at the bottom to see if we need to throw an exception

						int l_elen = pgStream.ReceiveIntegerR(4);
						String totalMessage = connection.getEncoding().decode(pgStream.Receive(l_elen-4));
						PSQLException l_error = PSQLException.parseServerError(totalMessage);

						if (error != null) {
							error.setNextException(l_error);
						} else {
							error = l_error;
						}

						// keep processing
						break;
					case 'I':	// Empty Query
						int t = pgStream.ReceiveIntegerR(4);
						break;
					case 'N':	// Error Notification
						int l_nlen = pgStream.ReceiveIntegerR(4);
						PSQLException notify = PSQLException.parseServerError(connection.getEncoding().decode(pgStream.Receive(l_nlen-4)));
						statement.addWarning(notify.getMessage());
						break;
					case 'P':	// Portal Name
						String pname = pgStream.ReceiveString(connection.getEncoding());
						break;
			        case 'S':
						//TODO: handle parameter status messages
						int l_len = pgStream.ReceiveIntegerR(4);
						String l_pStatus = connection.getEncoding().decode(pgStream.Receive(l_len-4));
						if (Driver.logDebug)
							Driver.debug("ParameterStatus="+ l_pStatus);
						break;
					case 'T':	// MetaData Field Description
						receiveFieldsV3();
						break;
					case 'Z':
						// read ReadyForQuery
						//TODO: use size better
						if (pgStream.ReceiveIntegerR(4) != 5) throw new PSQLException("postgresql.con.setup", PSQLState.CONNECTION_UNABLE_TO_CONNECT); 
						//TODO: handle transaction status
						char l_tStatus = (char)pgStream.ReceiveChar();
						l_endQuery = true;
						break;
					default:
						throw new PSQLException("postgresql.con.type", PSQLState.CONNECTION_FAILURE, new Character((char) c));
				}

			}

			// did we get an error during this query?
			if ( error != null )
				throw error;

			//if an existing result set was passed in reuse it, else
			//create a new one
			if (rs != null) 
			{
				rs.reInit(fields, tuples, status, update_count, insert_oid);
			}
			else 
			{
				rs = statement.createResultSet(fields, tuples, status, update_count, insert_oid);
			}
			return rs;
		}
	}

	private BaseResultSet executeV2() throws SQLException
	{

		StringBuffer errorMessage = null;

		if (pgStream == null) 
		{
			throw new PSQLException("postgresql.con.closed", PSQLState.CONNECTION_DOES_NOT_EXIST);
		}

		synchronized (pgStream)
		{

			sendQueryV2();

			int c;
			boolean l_endQuery = false;
			while (!l_endQuery)
			{
				c = pgStream.ReceiveChar();

				switch (c)
				{
					case 'A':	// Asynchronous Notify
						int pid = pgStream.ReceiveIntegerR(4);
						String msg = pgStream.ReceiveString(connection.getEncoding());
						connection.addNotification(new org.postgresql.core.Notification(msg, pid));
						break;
					case 'B':	// Binary Data Transfer
						receiveTupleV2(true);
						break;
					case 'C':	// Command Status
						receiveCommandStatusV2();
						break;
					case 'D':	// Text Data Transfer
						receiveTupleV2(false);
						break;
					case 'E':	// Error Message

						// it's possible to get more than one error message for a query
						// see libpq comments wrt backend closing a connection
						// so, append messages to a string buffer and keep processing
						// check at the bottom to see if we need to throw an exception

						if ( errorMessage == null )
							errorMessage = new StringBuffer();

						errorMessage.append(pgStream.ReceiveString(connection.getEncoding()));
						// keep processing
						break;
					case 'I':	// Empty Query
						int t = pgStream.ReceiveIntegerR(4);
						break;
					case 'N':	// Error Notification
						statement.addWarning(pgStream.ReceiveString(connection.getEncoding()));
						break;
					case 'P':	// Portal Name
						String pname = pgStream.ReceiveString(connection.getEncoding());
						break;
					case 'T':	// MetaData Field Description
						receiveFieldsV2();
						break;
					case 'Z':
						l_endQuery = true;
						break;
					default:
						throw new PSQLException("postgresql.con.type", PSQLState.CONNECTION_FAILURE, new Character((char) c));
				}

			}

			// did we get an error during this query?
			if ( errorMessage != null )
				throw new SQLException( errorMessage.toString().trim() );


			//if an existing result set was passed in reuse it, else
			//create a new one
			if (rs != null) 
			{
				rs.reInit(fields, tuples, status, update_count, insert_oid);
			}
			else 
			{
				rs = statement.createResultSet(fields, tuples, status, update_count, insert_oid);
			}
			return rs;
		}
	}

	/*
	 * Send a query to the backend.
	 */
	private void sendQueryV3() throws SQLException
	{
		for ( int i = 0; i < m_binds.length ; i++ )
		{
			if ( m_binds[i] == null )
				throw new PSQLException("postgresql.prep.param", PSQLState.INVALID_PARAMETER_VALUE, new Integer(i + 1));
		}
		try
		{
			byte[][] l_parts = new byte[(m_binds.length*2)+1][];
			int j = 0;
			int l_msgSize = 4;
			Encoding l_encoding = connection.getEncoding();
			pgStream.SendChar('Q');
			for (int i = 0 ; i < m_binds.length ; ++i)
			{
				l_parts[j] = l_encoding.encode(m_sqlFrags[i]);
				l_msgSize += l_parts[j].length;
				j++;
				l_parts[j] = l_encoding.encode(m_binds[i].toString());
				l_msgSize += l_parts[j].length;
				j++;
			}
			l_parts[j] = l_encoding.encode(m_sqlFrags[m_binds.length]);
			l_msgSize += l_parts[j].length;
			pgStream.SendInteger(l_msgSize+1,4);
			for (int k = 0; k < l_parts.length; k++) {
				pgStream.Send(l_parts[k]);
			}
			pgStream.SendChar(0);
			pgStream.flush();
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.ioerror", PSQLState.CONNECTION_FAILURE_DURING_TRANSACTION, e);
		}
	}

	/*
	 * Send a query to the backend.
	 */
	private void sendQueryV2() throws SQLException
	{
		for ( int i = 0; i < m_binds.length ; i++ )
		{
			if ( m_binds[i] == null )
				throw new PSQLException("postgresql.prep.param", PSQLState.INVALID_PARAMETER_VALUE, new Integer(i + 1));
		}
		try
		{
			pgStream.SendChar('Q');
			for (int i = 0 ; i < m_binds.length ; ++i)
			{
				pgStream.Send(connection.getEncoding().encode(m_sqlFrags[i]));
				pgStream.Send(connection.getEncoding().encode(m_binds[i].toString()));
			}

			pgStream.Send(connection.getEncoding().encode(m_sqlFrags[m_binds.length]));
			pgStream.SendChar(0);
			pgStream.flush();

		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.con.ioerror", PSQLState.CONNECTION_FAILURE_DURING_TRANSACTION, e);
		}
	}

	/*
	 * Receive a tuple from the backend.
	 */
	private void receiveTupleV3() throws SQLException
	{
		if (fields == null)
			throw new PSQLException("postgresql.con.tuple", PSQLState.CONNECTION_FAILURE);
		Object tuple = pgStream.ReceiveTupleV3(fields.length);
		if (maxRows == 0 || tuples.size() < maxRows)
			tuples.addElement(tuple);
	}

	/*
	 * Receive a tuple from the backend.
	 *
	 * @param isBinary set if the tuple should be treated as binary data
	 */
	private void receiveTupleV2(boolean isBinary) throws SQLException
	{
		if (fields == null)
			throw new PSQLException("postgresql.con.tuple", PSQLState.CONNECTION_FAILURE);
		Object tuple = pgStream.ReceiveTupleV2(fields.length, isBinary);
		if (isBinary) {
		    for (int i = 0; i < fields.length; i++) {
                        fields[i].setFormat(Field.BINARY_FORMAT); //Set the field to binary format
		    }
		}
		if (maxRows == 0 || tuples.size() < maxRows)
			tuples.addElement(tuple);
	}

	/*
	 * Receive command status from the backend.
	 */
	private void receiveCommandStatusV3() throws SQLException
	{
		//TODO: better handle the msg len
		int l_len = pgStream.ReceiveIntegerR(4);
		//read l_len -5 bytes (-4 for l_len and -1 for trailing \0)
		status = connection.getEncoding().decode(pgStream.Receive(l_len-5)); 
		//now read and discard the trailing \0
		pgStream.Receive(1);
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
			throw new PSQLException("postgresql.con.fathom", PSQLState.CONNECTION_FAILURE, status);
		}
	}
	/*
	 * Receive command status from the backend.
	 */
	private void receiveCommandStatusV2() throws SQLException
	{

		status = pgStream.ReceiveString(connection.getEncoding());

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
			throw new PSQLException("postgresql.con.fathom", PSQLState.CONNECTION_FAILURE, status);
		}
	}

	/*
	 * Receive the field descriptions from the back end.
	 */
	private void receiveFieldsV3() throws SQLException
	{
		//TODO: use the msgSize
		//TODO: use the tableOid, and tablePosition
		if (fields != null)
			throw new PSQLException("postgresql.con.multres", PSQLState.CONNECTION_FAILURE);
		int l_msgSize = pgStream.ReceiveIntegerR(4);
		int size = pgStream.ReceiveIntegerR(2);
		fields = new Field[size];

		for (int i = 0; i < fields.length; i++)
		{
			String typeName = pgStream.ReceiveString(connection.getEncoding());
			int tableOid = pgStream.ReceiveIntegerR(4);
			int tablePosition = pgStream.ReceiveIntegerR(2);
			int typeOid = pgStream.ReceiveIntegerR(4);
			int typeLength = pgStream.ReceiveIntegerR(2);
			int typeModifier = pgStream.ReceiveIntegerR(4);
			int formatType = pgStream.ReceiveIntegerR(2);
			//TODO: use the extra values coming back
			fields[i] = new Field(connection, typeName, typeOid, typeLength, typeModifier);
                        fields[i].setFormat(formatType);
		}
	}
	/*
	 * Receive the field descriptions from the back end.
	 */
	private void receiveFieldsV2() throws SQLException
	{
		if (fields != null)
			throw new PSQLException("postgresql.con.multres", PSQLState.CONNECTION_FAILURE);

		int size = pgStream.ReceiveIntegerR(2);
		fields = new Field[size];

		for (int i = 0; i < fields.length; i++)
		{
			String typeName = pgStream.ReceiveString(connection.getEncoding());
			int typeOid = pgStream.ReceiveIntegerR(4);
			int typeLength = pgStream.ReceiveIntegerR(2);
			int typeModifier = pgStream.ReceiveIntegerR(4);
			fields[i] = new Field(connection, typeName, typeOid, typeLength, typeModifier);
		}
	}
}
