package org.postgresql.jdbc2;

// IMPORTANT NOTE: This file implements the JDBC 2 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 1 class in the
// org.postgresql.jdbc1 package.

import java.io.*;
import java.lang.*;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import org.postgresql.Field;
import org.postgresql.fastpath.*;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;

/**
 * $Id: Connection.java,v 1.5 2001/01/18 17:37:14 peter Exp $
 *
 * A Connection represents a session with a specific database.  Within the
 * context of a Connection, SQL statements are executed and results are
 * returned.
 *
 * <P>A Connection's database is able to provide information describing
 * its tables, its supported SQL grammar, its stored procedures, the
 * capabilities of this connection, etc.  This information is obtained
 * with the getMetaData method.
 *
 * <p><B>Note:</B> By default, the Connection automatically commits changes
 * after executing each statement.  If auto-commit has been disabled, an
 * explicit commit must be done or database changes will not be saved.
 *
 * @see java.sql.Connection
 */
public class Connection extends org.postgresql.Connection implements java.sql.Connection
{
  // This is a cache of the DatabaseMetaData instance for this connection
  protected DatabaseMetaData metadata;

  protected java.util.Map typemap;

  /**
   * SQL statements without parameters are normally executed using
   * Statement objects.  If the same SQL statement is executed many
   * times, it is more efficient to use a PreparedStatement
   *
   * @return a new Statement object
   * @exception SQLException passed through from the constructor
   */
  public java.sql.Statement createStatement() throws SQLException
  {
    // The spec says default of TYPE_FORWARD_ONLY but everyone is used to
    // using TYPE_SCROLL_INSENSITIVE
    return createStatement(java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE,java.sql.ResultSet.CONCUR_READ_ONLY);
  }

  /**
   * SQL statements without parameters are normally executed using
   * Statement objects.  If the same SQL statement is executed many
   * times, it is more efficient to use a PreparedStatement
   *
   * @param resultSetType to use
   * @param resultSetCuncurrency to use
   * @return a new Statement object
   * @exception SQLException passed through from the constructor
   */
  public java.sql.Statement createStatement(int resultSetType,int resultSetConcurrency) throws SQLException
  {
    Statement s = new Statement(this);
    s.setResultSetType(resultSetType);
    s.setResultSetConcurrency(resultSetConcurrency);
    return s;
  }


  /**
   * A SQL statement with or without IN parameters can be pre-compiled
   * and stored in a PreparedStatement object.  This object can then
   * be used to efficiently execute this statement multiple times.
   *
   * <B>Note:</B> This method is optimized for handling parametric
   * SQL statements that benefit from precompilation if the drivers
   * supports precompilation.  PostgreSQL does not support precompilation.
   * In this case, the statement is not sent to the database until the
   * PreparedStatement is executed.  This has no direct effect on users;
   * however it does affect which method throws certain SQLExceptions
   *
   * @param sql a SQL statement that may contain one or more '?' IN
   *	parameter placeholders
   * @return a new PreparedStatement object containing the pre-compiled
   *	statement.
   * @exception SQLException if a database access error occurs.
   */
  public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException
  {
    return prepareStatement(sql,java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE,java.sql.ResultSet.CONCUR_READ_ONLY);
  }

  public java.sql.PreparedStatement prepareStatement(String sql,int resultSetType,int resultSetConcurrency) throws SQLException
  {
    PreparedStatement s = new PreparedStatement(this,sql);
    s.setResultSetType(resultSetType);
    s.setResultSetConcurrency(resultSetConcurrency);
    return s;
  }

  /**
   * A SQL stored procedure call statement is handled by creating a
   * CallableStatement for it.  The CallableStatement provides methods
   * for setting up its IN and OUT parameters and methods for executing
   * it.
   *
   * <B>Note:</B> This method is optimised for handling stored procedure
   * call statements.  Some drivers may send the call statement to the
   * database when the prepareCall is done; others may wait until the
   * CallableStatement is executed.  This has no direct effect on users;
   * however, it does affect which method throws certain SQLExceptions
   *
   * @param sql a SQL statement that may contain one or more '?' parameter
   *	placeholders.  Typically this statement is a JDBC function call
   *	escape string.
   * @return a new CallableStatement object containing the pre-compiled
   *	SQL statement
   * @exception SQLException if a database access error occurs
   */
  public java.sql.CallableStatement prepareCall(String sql) throws SQLException
  {
    return prepareCall(sql,java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE,java.sql.ResultSet.CONCUR_READ_ONLY);
  }

  public java.sql.CallableStatement prepareCall(String sql,int resultSetType,int resultSetConcurrency) throws SQLException
  {
    throw new PSQLException("postgresql.con.call");
    //CallableStatement s = new CallableStatement(this,sql);
    //s.setResultSetType(resultSetType);
    //s.setResultSetConcurrency(resultSetConcurrency);
    //return s;
  }


  /**
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

  /**
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
   * values.  Here the commit occurs when all results and output param
   * values have been retrieved.
   *
   * @param autoCommit - true enables auto-commit; false disables it
   * @exception SQLException if a database access error occurs
   */
  public void setAutoCommit(boolean autoCommit) throws SQLException
  {
    if (this.autoCommit == autoCommit)
      return;
    if (autoCommit)
      ExecSQL("end");
    else
      ExecSQL("begin");
    this.autoCommit = autoCommit;
  }

  /**
   * gets the current auto-commit state
   *
   * @return Current state of the auto-commit mode
   * @exception SQLException (why?)
   * @see setAutoCommit
   */
  public boolean getAutoCommit() throws SQLException
  {
    return this.autoCommit;
  }

  /**
   * The method commit() makes all changes made since the previous
   * commit/rollback permanent and releases any database locks currently
   * held by the Connection.  This method should only be used when
   * auto-commit has been disabled.  (If autoCommit == true, then we
   * just return anyhow)
   *
   * @exception SQLException if a database access error occurs
   * @see setAutoCommit
   */
  public void commit() throws SQLException
  {
    if (autoCommit)
      return;
    ExecSQL("commit");
    autoCommit = true;
    ExecSQL("begin");
    autoCommit = false;
  }

  /**
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
      return;
    ExecSQL("rollback");
    autoCommit = true;
    ExecSQL("begin");
    autoCommit = false;
  }

  /**
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
    if (pg_stream != null)
      {
	try
	  {
	    pg_stream.close();
	  } catch (IOException e) {}
	  pg_stream = null;
      }
  }

  /**
   * Tests to see if a Connection is closed
   *
   * @return the status of the connection
   * @exception SQLException (why?)
   */
  public boolean isClosed() throws SQLException
  {
    return (pg_stream == null);
  }

  /**
   * A connection's database is able to provide information describing
   * its tables, its supported SQL grammar, its stored procedures, the
   * capabilities of this connection, etc.  This information is made
   * available through a DatabaseMetaData object.
   *
   * @return a DatabaseMetaData object for this connection
   * @exception SQLException if a database access error occurs
   */
  public java.sql.DatabaseMetaData getMetaData() throws SQLException
  {
    if(metadata==null)
      metadata = new DatabaseMetaData(this);
    return metadata;
  }

  /**
   * You can put a connection in read-only mode as a hunt to enable
   * database optimizations
   *
   * <B>Note:</B> setReadOnly cannot be called while in the middle
   * of a transaction
   *
   * @param readOnly - true enables read-only mode; false disables it
   * @exception SQLException if a database access error occurs
   */
  public void setReadOnly (boolean readOnly) throws SQLException
  {
    this.readOnly = readOnly;
  }

  /**
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

  /**
   * A sub-space of this Connection's database may be selected by
   * setting a catalog name.  If the driver does not support catalogs,
   * it will silently ignore this request
   *
   * @exception SQLException if a database access error occurs
   */
  public void setCatalog(String catalog) throws SQLException
  {
    // No-op
  }

  /**
   * Return the connections current catalog name, or null if no
   * catalog name is set, or we dont support catalogs.
   *
   * @return the current catalog name or null
   * @exception SQLException if a database access error occurs
   */
  public String getCatalog() throws SQLException
  {
    return null;
  }

  /**
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
    String q = "SET TRANSACTION ISOLATION LEVEL";

    switch(level) {

      case java.sql.Connection.TRANSACTION_READ_COMMITTED:
        ExecSQL(q + " READ COMMITTED");
	return;

      case java.sql.Connection.TRANSACTION_SERIALIZABLE:
        ExecSQL(q + " SERIALIZABLE");
	return;

      default:
        throw new PSQLException("postgresql.con.isolevel",new Integer(level));
    }
  }

  /**
   * Get this Connection's current transaction isolation mode.
   *
   * @return the current TRANSACTION_* mode value
   * @exception SQLException if a database access error occurs
   */
  public int getTransactionIsolation() throws SQLException
  {
      ExecSQL("show xactisolevel");

      SQLWarning w = getWarnings();
      if (w != null) {
	  if (w.getMessage().indexOf("READ COMMITTED") != -1) return java.sql.Connection.TRANSACTION_READ_COMMITTED; else
	      if (w.getMessage().indexOf("READ UNCOMMITTED") != -1) return java.sql.Connection.TRANSACTION_READ_UNCOMMITTED; else
		  if (w.getMessage().indexOf("REPEATABLE READ") != -1) return java.sql.Connection.TRANSACTION_REPEATABLE_READ; else
		      if (w.getMessage().indexOf("SERIALIZABLE") != -1) return java.sql.Connection.TRANSACTION_SERIALIZABLE;
      }
      return java.sql.Connection.TRANSACTION_READ_COMMITTED;
  }

  /**
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

  /**
   * After this call, getWarnings returns null until a new warning
   * is reported for this connection.
   *
   * @exception SQLException if a database access error occurs
   */
  public void clearWarnings() throws SQLException
  {
    firstWarning = null;
  }

    /**
     * This overides the method in org.postgresql.Connection and returns a
     * ResultSet.
     */
    protected java.sql.ResultSet getResultSet(org.postgresql.Connection conn, java.sql.Statement stat,Field[] fields, Vector tuples, String status, int updateCount, int insertOID) throws SQLException
    {
      // In 7.1 we now test concurrency to see which class to return. If we are not working with a
      // Statement then default to a normal ResultSet object.
      if(stat!=null) {
        if(stat.getResultSetConcurrency()==java.sql.ResultSet.CONCUR_UPDATABLE)
          return new org.postgresql.jdbc2.UpdateableResultSet((org.postgresql.jdbc2.Connection)conn,fields,tuples,status,updateCount,insertOID);
      }

      return new org.postgresql.jdbc2.ResultSet((org.postgresql.jdbc2.Connection)conn,fields,tuples,status,updateCount,insertOID);
    }

    // *****************
    // JDBC 2 extensions
    // *****************

    public java.util.Map getTypeMap() throws SQLException
    {
      // new in 7.1
      return typemap;
    }


    public void setTypeMap(java.util.Map map) throws SQLException
    {
      // new in 7.1
      typemap=map;
    }

    /**
     * This overides the standard internal getObject method so that we can
     * check the jdbc2 type map first
     *
     * @return PGobject for this type, and set to value
     * @exception SQLException if value is not correct for this type
     * @see org.postgresql.util.Serialize
     */
    public Object getObject(String type,String value) throws SQLException
    {
      if(typemap!=null) {
        SQLData d = (SQLData) typemap.get(type);
        if(d!=null) {
          // Handle the type (requires SQLInput & SQLOutput classes to be implemented)
          throw org.postgresql.Driver.notImplemented();
        }
      }

      // Default to the original method
      return super.getObject(type,value);
    }
}

// ***********************************************************************

