package org.postgresql.jdbc2;

// IMPORTANT NOTE: This file implements the JDBC 2 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 1 class in the
// org.postgresql.jdbc1 package.

import java.lang.*;
import java.sql.*;
import java.util.*;
import org.postgresql.*;
import org.postgresql.util.*;

/**
 * A ResultSetMetaData object can be used to find out about the types and
 * properties of the columns in a ResultSet
 *
 * @see java.sql.ResultSetMetaData
 */
public class ResultSetMetaData implements java.sql.ResultSetMetaData 
{
  Vector rows;
  Field[] fields;
  
  /**
   *	Initialise for a result with a tuple set and
   *	a field descriptor set
   *
   * @param rows the Vector of rows returned by the ResultSet
   * @param fields the array of field descriptors
   */
  public ResultSetMetaData(Vector rows, Field[] fields)
  {
    this.rows = rows;
    this.fields = fields;
  }
  
  /**
   * Whats the number of columns in the ResultSet?
   *
   * @return the number
   * @exception SQLException if a database access error occurs
   */
  public int getColumnCount() throws SQLException
  {
    return fields.length;
  }
  
  /**
   * Is the column automatically numbered (and thus read-only)
   * I believe that PostgreSQL does not support this feature.
   *
   * @param column the first column is 1, the second is 2...
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isAutoIncrement(int column) throws SQLException
  {
    return false;
  }
  
  /**
   * Does a column's case matter? ASSUMPTION: Any field that is
   * not obviously case insensitive is assumed to be case sensitive
   *
   * @param column the first column is 1, the second is 2...
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isCaseSensitive(int column) throws SQLException
  {
    int sql_type = getField(column).getSQLType();
    
    switch (sql_type)
      {
      case Types.SMALLINT:
      case Types.INTEGER:
      case Types.FLOAT:
      case Types.REAL:
      case Types.DOUBLE:
      case Types.DATE:
      case Types.TIME:
      case Types.TIMESTAMP:
	return false;
      default:
	return true;
      }
  }
  
  /**
   * Can the column be used in a WHERE clause?  Basically for
   * this, I split the functions into two types: recognised
   * types (which are always useable), and OTHER types (which
   * may or may not be useable).  The OTHER types, for now, I
   * will assume they are useable.  We should really query the
   * catalog to see if they are useable.
   *
   * @param column the first column is 1, the second is 2...
   * @return true if they can be used in a WHERE clause
   * @exception SQLException if a database access error occurs
   */
  public boolean isSearchable(int column) throws SQLException
  {
    int sql_type = getField(column).getSQLType();
    
    // This switch is pointless, I know - but it is a set-up
    // for further expansion.		
    switch (sql_type)
      {
      case Types.OTHER:
	return true;
      default:
	return true;
      }
  }
  
  /**
   * Is the column a cash value?  6.1 introduced the cash/money
   * type, which haven't been incorporated as of 970414, so I
   * just check the type name for both 'cash' and 'money'
   *
   * @param column the first column is 1, the second is 2...
   * @return true if its a cash column
   * @exception SQLException if a database access error occurs
   */
  public boolean isCurrency(int column) throws SQLException
  {
    String type_name = getField(column).getTypeName();
    
    return type_name.equals("cash") || type_name.equals("money");
  }
  
  /**
   * Can you put a NULL in this column?  I think this is always
   * true in 6.1's case.  It would only be false if the field had
   * been defined NOT NULL (system catalogs could be queried?)
   *
   * @param column the first column is 1, the second is 2...
   * @return one of the columnNullable values
   * @exception SQLException if a database access error occurs
   */
  public int isNullable(int column) throws SQLException
  {
    return columnNullable;	// We can always put NULL in
  }
  
  /**
   * Is the column a signed number? In PostgreSQL, all numbers
   * are signed, so this is trivial.  However, strings are not
   * signed (duh!)
   * 
   * @param column the first column is 1, the second is 2...
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isSigned(int column) throws SQLException
  {
    int sql_type = getField(column).getSQLType();
    
    switch (sql_type)
      {
      case Types.SMALLINT:
      case Types.INTEGER:
      case Types.FLOAT:
      case Types.REAL:
      case Types.DOUBLE:
	return true;
      case Types.DATE:
      case Types.TIME:
      case Types.TIMESTAMP:
	return false;	// I don't know about these?
      default:
	return false;
      }
  }
  
  /**
   * What is the column's normal maximum width in characters?
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the maximum width
   * @exception SQLException if a database access error occurs
   */
  public int getColumnDisplaySize(int column) throws SQLException
  {
    Field f = getField(column);
    String type_name = f.getTypeName();
    int sql_type = f.getSQLType();
    int typmod = f.mod;

    // I looked at other JDBC implementations and couldn't find a consistent
    // interpretation of the "display size" for numeric values, so this is our's
    // FIXME: currently, only types with a SQL92 or SQL3 pendant are implemented - jens@jens.de

    // fixed length data types
    if (type_name.equals( "int2"      ))  return 6;  // -32768 to +32768 (5 digits and a sign)
    if (type_name.equals( "int4"      ) 
     || type_name.equals( "oid"       ))  return 11; // -2147483648 to +2147483647
    if (type_name.equals( "int8"      ))  return 20; // -9223372036854775808 to +9223372036854775807
    if (type_name.equals( "money"     ))  return 12; // MONEY = DECIMAL(9,2)
    if (type_name.equals( "float4"    ))  return 11; // i checked it out ans wasn't able to produce more than 11 digits
    if (type_name.equals( "float8"    ))  return 20; // dito, 20
    if (type_name.equals( "char"      ))  return 1;
    if (type_name.equals( "bool"      ))  return 1;
    if (type_name.equals( "date"      ))  return 14; // "01/01/4713 BC" - "31/12/32767 AD"
    if (type_name.equals( "time"      ))  return 8;  // 00:00:00-23:59:59
    if (type_name.equals( "timestamp" ))  return 22; // hhmmm ... the output looks like this: 1999-08-03 22:22:08+02

    // variable length fields
    typmod -= 4;
    if (type_name.equals( "bpchar"    )
     || type_name.equals( "varchar"   ))  return typmod; // VARHDRSZ=sizeof(int32)=4
    if (type_name.equals( "numeric"   ))  return ( (typmod >>16) & 0xffff )
                                           + 1 + ( typmod        & 0xffff ); // DECIMAL(p,s) = (p digits).(s digits)

    // if we don't know better
    return f.length;
  }
  
  /**
   * What is the suggested column title for use in printouts and
   * displays?  We suggest the ColumnName!
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the column label
   * @exception SQLException if a database access error occurs
   */
  public String getColumnLabel(int column) throws SQLException
  {
    return getColumnName(column);
  }
  
  /**
   * What's a column's name?
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the column name
   * @exception SQLException if a database access error occurs
   */
  public String getColumnName(int column) throws SQLException
  {
    Field f = getField(column);
    if(f!=null)
      return f.name;
    return "field"+column;
  }
  
  /**
   * What is a column's table's schema?  This relies on us knowing
   * the table name....which I don't know how to do as yet.  The 
   * JDBC specification allows us to return "" if this is not
   * applicable.
   *
   * @param column the first column is 1, the second is 2...
   * @return the Schema
   * @exception SQLException if a database access error occurs
   */
  public String getSchemaName(int column) throws SQLException
  {
    return "";
  }
  
  /**
   * What is a column's number of decimal digits.
   *
   * @param column the first column is 1, the second is 2...
   * @return the precision
   * @exception SQLException if a database access error occurs
   */
  public int getPrecision(int column) throws SQLException
  {
    int sql_type = getField(column).getSQLType();
    
    switch (sql_type)
      {
      case Types.SMALLINT:
	return 5;	
      case Types.INTEGER:
	return 10;
      case Types.REAL:
	return 8;
      case Types.FLOAT:
	return 16;
      case Types.DOUBLE:
	return 16;
      case Types.VARCHAR:
	return 0;
      default:
	return 0;
      }
  }
  
  /**
   * What is a column's number of digits to the right of the
   * decimal point?
   *
   * @param column the first column is 1, the second is 2...
   * @return the scale
   * @exception SQLException if a database access error occurs
   */
  public int getScale(int column) throws SQLException
  {
    int sql_type = getField(column).getSQLType();
    
    switch (sql_type)
      {
      case Types.SMALLINT:
	return 0;
      case Types.INTEGER:
	return 0;
      case Types.REAL:
	return 8;
      case Types.FLOAT:
	return 16;
      case Types.DOUBLE:
	return 16;
      case Types.VARCHAR:
	return 0;
      default:
	return 0;
      }
  }
  
  /**
   * Whats a column's table's name?  How do I find this out?  Both
   * getSchemaName() and getCatalogName() rely on knowing the table
   * Name, so we need this before we can work on them.
   *
   * @param column the first column is 1, the second is 2...
   * @return column name, or "" if not applicable
   * @exception SQLException if a database access error occurs
   */
  public String getTableName(int column) throws SQLException
  {
    return "";
  }
  
  /**
   * What's a column's table's catalog name?  As with getSchemaName(),
   * we can say that if getTableName() returns n/a, then we can too -
   * otherwise, we need to work on it.
   * 
   * @param column the first column is 1, the second is 2...
   * @return catalog name, or "" if not applicable
   * @exception SQLException if a database access error occurs
   */
  public String getCatalogName(int column) throws SQLException
  {
    return "";
  }
  
  /**
   * What is a column's SQL Type? (java.sql.Type int)
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the java.sql.Type value
   * @exception SQLException if a database access error occurs
   * @see org.postgresql.Field#getSQLType
   * @see java.sql.Types
   */
  public int getColumnType(int column) throws SQLException
  {
    return getField(column).getSQLType();
  }
  
  /**
   * Whats is the column's data source specific type name?
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the type name
   * @exception SQLException if a database access error occurs
   */
  public String getColumnTypeName(int column) throws SQLException
  {
    return getField(column).getTypeName();
  }
  
  /**
   * Is the column definitely not writable?  In reality, we would
   * have to check the GRANT/REVOKE stuff for this to be effective,
   * and I haven't really looked into that yet, so this will get
   * re-visited.
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isReadOnly(int column) throws SQLException
  {
    return false;
  }
  
  /**
   * Is it possible for a write on the column to succeed?  Again, we
   * would in reality have to check the GRANT/REVOKE stuff, which
   * I haven't worked with as yet.  However, if it isn't ReadOnly, then
   * it is obviously writable.
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isWritable(int column) throws SQLException
  {
    if (isReadOnly(column))
      return true;
    else
      return false;
  }
  
  /**
   * Will a write on this column definately succeed?  Hmmm...this
   * is a bad one, since the two preceding functions have not been
   * really defined.  I cannot tell is the short answer.  I thus
   * return isWritable() just to give us an idea.
   *
   * @param column the first column is 1, the second is 2, etc..
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isDefinitelyWritable(int column) throws SQLException
  {
    return isWritable(column);
  }
  
  // ********************************************************
  // 	END OF PUBLIC INTERFACE
  // ********************************************************
  
  /**
   * For several routines in this package, we need to convert
   * a columnIndex into a Field[] descriptor.  Rather than do
   * the same code several times, here it is.
   * 
   * @param columnIndex the first column is 1, the second is 2...
   * @return the Field description
   * @exception SQLException if a database access error occurs
   */
  private Field getField(int columnIndex) throws SQLException
  {
    if (columnIndex < 1 || columnIndex > fields.length)
      throw new PSQLException("postgresql.res.colrange");
    return fields[columnIndex - 1];
  }
    
    // ** JDBC 2 Extensions **
    
    // This can hook into our PG_Object mechanism
    public String getColumnClassName(int column) throws SQLException
    {
	throw org.postgresql.Driver.notImplemented();
    }
    
}

