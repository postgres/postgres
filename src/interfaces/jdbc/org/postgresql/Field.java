package org.postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import org.postgresql.*;
import org.postgresql.util.*;

/**
 * org.postgresql.Field is a class used to describe fields in a PostgreSQL
 * ResultSet
 */
public class Field
{
  public int length;		// Internal Length of this field
  public int oid;		// OID of the type
  public int mod;		// type modifier of this field
  public String name;		// Name of this field
  
  protected Connection conn;	// Connection Instantation
  
  public int sql_type = -1;	// The entry in java.sql.Types for this field
  public String type_name = null;// The sql type name
  
  /**
   * Construct a field based on the information fed to it.
   *
   * @param conn the connection this field came from
   * @param name the name of the field
   * @param oid the OID of the field
   * @param len the length of the field
   */
  public Field(Connection conn, String name, int oid, int length,int mod)
  {
    this.conn = conn;
    this.name = name;
    this.oid = oid;
    this.length = length;
    this.mod = mod;
  }
  
    /**
     * Constructor without mod parameter.
     *
     * @param conn the connection this field came from
     * @param name the name of the field
     * @param oid the OID of the field
     * @param len the length of the field
     */
    public Field(Connection conn, String name, int oid, int length)
    {
	this(conn,name,oid,length,0);
    }
    
  /**
   * @return the oid of this Field's data type
   */
  public int getOID()
  {
    return oid;
  }
  
  /**
   * the ResultSet and ResultMetaData both need to handle the SQL
   * type, which is gained from another query.  Note that we cannot
   * use getObject() in this, since getObject uses getSQLType().
   *
   * @return the entry in Types that refers to this field
   * @exception SQLException if a database access error occurs
   */
  public int getSQLType() throws SQLException
  {
    if(sql_type == -1) {
      type_name = (String)conn.fieldCache.get(new Integer(oid));
      
      // it's not in the cache, so perform a query, and add the result to
      // the cache
      if(type_name==null) {
	ResultSet result = (org.postgresql.ResultSet)
	    conn.ExecSQL(null, "select typname from pg_type where oid = " 
			 + oid);
	if (result.getColumnCount() != 1 || result.getTupleCount() != 1)
	  throw new PSQLException("postgresql.unexpected");
	result.next();
	type_name = result.getString(1);
	conn.fieldCache.put(new Integer(oid),type_name);
	result.close();
      }
      
      sql_type = getSQLType(type_name);
    }
    return sql_type;
  }
  
  /**
   * This returns the SQL type. It is called by the Field and DatabaseMetaData classes
   * @param type_name PostgreSQL type name
   * @return java.sql.Types value for oid
   */
  public static int getSQLType(String type_name)
  {
    int sql_type = Types.OTHER; // default value
    for(int i=0;i<types.length;i++)
      if(type_name.equals(types[i]))
	sql_type=typei[i];
    return sql_type;
  }
  
  /**
   * This table holds the org.postgresql names for the types supported.
   * Any types that map to Types.OTHER (eg POINT) don't go into this table.
   * They default automatically to Types.OTHER
   *
   * Note: This must be in the same order as below.
   *
   * Tip: keep these grouped together by the Types. value
   */
  private static final String types[] = {
    "int2",
    "int4","oid",
    "int8",
    "cash","money",
    "numeric",
    "float4",
    "float8",
    "bpchar","char","char2","char4","char8","char16",
    "varchar","text","name","filename",
    "bool",
    "date",
    "time",
    "abstime","timestamp"
  };
  
  /**
   * This table holds the JDBC type for each entry above.
   *
   * Note: This must be in the same order as above
   *
   * Tip: keep these grouped together by the Types. value
   */
  private static final int typei[] = {
    Types.SMALLINT,
    Types.INTEGER,Types.INTEGER,
    Types.BIGINT,
    Types.DECIMAL,Types.DECIMAL,
    Types.NUMERIC,
    Types.REAL,
    Types.DOUBLE,
    Types.CHAR,Types.CHAR,Types.CHAR,Types.CHAR,Types.CHAR,Types.CHAR,
    Types.VARCHAR,Types.VARCHAR,Types.VARCHAR,Types.VARCHAR,
    Types.BIT,
    Types.DATE,
    Types.TIME,
    Types.TIMESTAMP,Types.TIMESTAMP
  };
  
  /**
   * We also need to get the type name as returned by the back end.
   * This is held in type_name AFTER a call to getSQLType.  Since
   * we get this information within getSQLType (if it isn't already
   * done), we can just call getSQLType and throw away the result.
   *
   * @return the String representation of the type of this field
   * @exception SQLException if a database access error occurs
   */
  public String getTypeName() throws SQLException
  {
    int sql = getSQLType();
    return type_name;
  }
}
