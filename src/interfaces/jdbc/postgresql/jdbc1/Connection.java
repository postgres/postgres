package postgresql.jdbc1;

// IMPORTANT NOTE: This file implements the JDBC 1 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 2 class in the
// postgresql.jdbc2 package.

import java.io.*;
import java.lang.*;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import postgresql.Field;
import postgresql.fastpath.*;
import postgresql.largeobject.*;
import postgresql.util.*;

/**
 * $Id: Connection.java,v 1.1 1999/01/17 04:51:53 momjian Exp $
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
public class Connection extends postgresql.Connection implements java.sql.Connection 
{
  // This is a cache of the DatabaseMetaData instance for this connection
  protected DatabaseMetaData metadata;
  
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
    return new Statement(this);
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
    return new PreparedStatement(this, sql);
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
    throw new SQLException("Callable Statements are not supported at this time");
    //		return new CallableStatement(this, sql);
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
    throw new SQLException("Transaction Isolation Levels are not implemented");
  }
  
  /**
   * Get this Connection's current transaction isolation mode.
   * 
   * @return the current TRANSACTION_* mode value
   * @exception SQLException if a database access error occurs
   */
  public int getTransactionIsolation() throws SQLException
  {
    return java.sql.Connection.TRANSACTION_SERIALIZABLE;
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
     * This overides the method in postgresql.Connection and returns a
     * ResultSet.
     */
    protected java.sql.ResultSet getResultSet(postgresql.Connection conn, Field[] fields, Vector tuples, String status, int updateCount) throws SQLException
    {
	return new postgresql.jdbc1.ResultSet((postgresql.jdbc1.Connection)conn,fields,tuples,status,updateCount);
    }
    
}

// ***********************************************************************

