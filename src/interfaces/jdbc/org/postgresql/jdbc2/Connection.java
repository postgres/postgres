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
 * $Id: Connection.java,v 1.9 2001/07/30 14:51:19 momjian Exp $
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

  /**
   * The current type mappings
   */
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
   * Tests to see if a Connection is closed.
   *
   * Peter Feb 7 2000: Now I've discovered that this doesn't actually obey the
   * specifications. Under JDBC2.1, this should only be valid _after_ close()
   * has been called. It's result is not guraranteed to be valid before, and
   * client code should not use it to see if a connection is open. The spec says
   * that the client should monitor the SQLExceptions thrown when their queries
   * fail because the connection is dead.
   *
   * I don't like this definition. As it doesn't hurt breaking it here, our
   * isClosed() implementation does test the connection, so for PostgreSQL, you
   * can rely on isClosed() returning a valid result.
   *
   * @return the status of the connection
   * @exception SQLException (why?)
   */
  public boolean isClosed() throws SQLException
  {
    // If the stream is gone, then close() was called
    if(pg_stream == null)
      return true;

    // ok, test the connection
    try {
      // by sending an empty query. If we are dead, then an SQLException should
      // be thrown
      java.sql.ResultSet rs = ExecSQL(" ");
      if(rs!=null)
        rs.close();

      // By now, we must be alive
      return false;
    } catch(SQLException se) {
      // Why throw an SQLException as this may fail without throwing one,
      // ie isClosed() is called incase the connection has died, and we don't
      // want to find out by an Exception, so instead we return true, as its
      // most likely why it was thrown in the first place.
      return true;
    }
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

