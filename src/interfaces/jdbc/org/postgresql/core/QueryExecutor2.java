
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
 * $Id: QueryExecutor2.java,v 1.1 2002/03/21 03:20:29 davec Exp $
 */

public class QueryExecutor2
{

	private final String sql;
	private final java.sql.Statement statement;
	private final PG_Stream pg_stream;
	private final org.postgresql.Connection connection;

	public QueryExecutor2(String sql,
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

    StringBuffer errorMessage = null;

		synchronized (pg_stream)
		{

			sendQuery(sql);
      connection.asyncStatus = org.postgresql.Connection.PGASYNC_BUSY;

			while ( connection.asyncStatus != org.postgresql.Connection.PGASYNC_IDLE )
			{
				int c = pg_stream.ReceiveChar();

        if ( c == 'A' )
        {

          int pid = pg_stream.ReceiveIntegerR(4);
          String msg = pg_stream.ReceiveString(connection.getEncoding());

          org.postgresql.Driver.debug(msg);
          continue;
        }
        else if ( c == 'N' )
        {
          String notification = pg_stream.ReceiveString(connection.getEncoding());
          org.postgresql.Driver.debug(notification);
          connection.addWarning(notification);
          continue;
        }
        else if ( connection.asyncStatus != org.postgresql.Connection.PGASYNC_BUSY )
        {
          if ( connection.asyncStatus != org.postgresql.Connection.PGASYNC_IDLE )
          {
            // only one possibility left which is PGASYNC_READY, so let's get out
            break;
          }
          if ( c == 'E' ) {
            String error = pg_stream.ReceiveString(connection.getEncoding());
            org.postgresql.Driver.debug(error);

            // no sense in creating this object until we really need it
            if ( errorMessage == null ) {
              errorMessage = new StringBuffer();
            }

            errorMessage.append(error);
            break;
          }
        }else{

          switch (c)
          {
            case 'C':	// Command Status
              receiveCommandStatus();
              break;

            case 'E':	// Error Message

              // it's possible to get multiple error messages from one query
              // see libpq, there are some comments about a connection being closed
              // by the backend after real error occurs, so append error messages here
              // so append them and just remember that an error occured
              // throw the exception at the end of processing

              String error = pg_stream.ReceiveString(connection.getEncoding());
              org.postgresql.Driver.debug(error);

              // no sense in creating this object until we really need it
              if ( errorMessage == null ) {
                errorMessage = new StringBuffer();
              }

              errorMessage.append(error);
              connection.asyncStatus = org.postgresql.Connection.PGASYNC_READY;
              break;

            case 'Z':		 // backend ready for query, ignore for now :-)
              connection.asyncStatus = org.postgresql.Connection.PGASYNC_IDLE;
              break;

            case 'I':	// Empty Query
              int t = pg_stream.ReceiveChar();
              if (t != 0)
                throw new PSQLException("postgresql.con.garbled");

              connection.asyncStatus = org.postgresql.Connection.PGASYNC_READY;
              break;

            case 'P':	// Portal Name
              String pname = pg_stream.ReceiveString(connection.getEncoding());
              org.postgresql.Driver.debug(pname);
              break;

            case 'T':	// MetaData Field Description
              receiveFields();
              break;

            case 'B':	// Binary Data Transfer
              receiveTuple(true);
              break;

            case 'D':	// Text Data Transfer
              receiveTuple(false);
              break;

           default:
              throw new PSQLException("postgresql.con.type",
                          new Character((char) c));
          }
        }
			}
      // did we get an error message?

      if ( errorMessage != null ) {
        // yes, throw an exception
        throw new SQLException(errorMessage.toString());
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
