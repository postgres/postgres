package postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import postgresql.*;

/**
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 *
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
    
    if (type_name.equals("cash"))
      return true;
    if (type_name.equals("money"))
      return true;
    return false;
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
    int max = getColumnLabel(column).length();
    int i;
    
    for (i = 0 ; i < rows.size(); ++i)
      {
	byte[][] x = (byte[][])(rows.elementAt(i));
	int xl = x[column - 1].length;
	if (xl > max)
	  max = xl;
      }
    return max;
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
   * @exception SQLException if a databvase access error occurs
   */
  public String getColumnName(int column) throws SQLException
  {
    return getField(column).name;
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
    String table_name = getTableName(column);
    
    // If the table name is invalid, so are we.
    if (table_name.equals(""))
      return "";	
    return "";		// Ok, so I don't know how to
    // do this as yet.
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
      default:
	throw new SQLException("no precision for non-numeric data types.");
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
      default:
	throw new SQLException("no scale for non-numeric data types");
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
    String table_name = getTableName(column);
    
    if (table_name.equals(""))
      return "";
    return "";		// As with getSchemaName(), this
    // is just the start of it.
  }
  
  /**
   * What is a column's SQL Type? (java.sql.Type int)
   *
   * @param column the first column is 1, the second is 2, etc.
   * @return the java.sql.Type value
   * @exception SQLException if a database access error occurs
   * @see postgresql.Field#getSQLType
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
      throw new SQLException("Column index out of range");
    return fields[columnIndex - 1];
  }
}

