package org.postgresql.jdbc2;

import java.text.*;
import java.sql.*;
import java.util.*;
import java.math.BigDecimal;
import org.postgresql.Field;
import org.postgresql.util.*;

/**
 * Array is used collect one column of query result data.
 *
 * <p>Read a field of type Array into either a natively-typed
 * Java array object or a ResultSet.  Accessor methods provide
 * the ability to capture array slices.
 *
 * <p>Other than the constructor all methods are direct implementations
 * of those specified for java.sql.Array.  Please refer to the javadoc
 * for java.sql.Array for detailed descriptions of the functionality
 * and parameters of the methods of this class.
 *
 * @see ResultSet#getArray
 */


public class Array implements java.sql.Array
{
	private org.postgresql.Connection      conn   = null;
	private org.postgresql.Field           field  = null;
	private org.postgresql.jdbc2.ResultSet rs     = null;
	private int                            idx    = 0;

    /**
     * Create a new Array 
     *
     * @param conn a database connection
     * @param idx 1-based index of the query field to load into this Array
     * @param field the Field descriptor for the field to load into this Array
     * @param rs the ResultSet from which to get the data for this Array
     */
	public Array( org.postgresql.Connection conn, int idx, Field field, org.postgresql.jdbc2.ResultSet rs ) { 
		this.conn = conn;
		this.field = field; 
		this.rs = rs;
		this.idx = idx;
	}

	public Object getArray() throws SQLException {
		return getArray( 1, 0, null );
	}

	public Object getArray(long index, int count) throws SQLException {
		return getArray( index, count, null );
	}

	public Object getArray(Map map) throws SQLException {
		return getArray( 1, 0, map );
	}

	public Object getArray(long index, int count, Map map) throws SQLException {
		if( map != null ) // For now maps aren't supported.
      		throw org.postgresql.Driver.notImplemented();

    	if (index < 1)
      		throw new PSQLException("postgresql.arr.range");
		Object retVal = null;

		ArrayList array = new ArrayList();
		String raw = rs.getFixedString(idx);
		if( raw != null ) {
			char[] chars = raw.toCharArray();
			StringBuffer sbuf = new StringBuffer();
			boolean foundOpen = false;
			boolean insideString = false;
			for( int i=0; i<chars.length; i++ ) {
				if( chars[i] == '{' ) {
					if( foundOpen )  // Only supports 1-D arrays for now
						throw org.postgresql.Driver.notImplemented();
					foundOpen = true;
					continue;
				}
				if( chars[i] == '"' ) {
					insideString = !insideString;
					continue;
				}
				if( (!insideString && chars[i] == ',') || chars[i] == '}' || i == chars.length-1) {
					if( chars[i] != '"' && chars[i] != '}' && chars[i] != ',' )
						sbuf.append(chars[i]);
					array.add( sbuf.toString() );
					sbuf = new StringBuffer();
					continue;
				}
				sbuf.append( chars[i] );
			}
		}
		String[] arrayContents = (String[]) array.toArray( new String[array.size()] );
		if( count == 0 )
			count = arrayContents.length;
		index--;
		if( index+count > arrayContents.length )
      		throw new PSQLException("postgresql.arr.range");

		int i = 0;
		switch ( getBaseType() )
		{
			case Types.BIT:
				retVal = new boolean[ count ];
				for( ; count > 0; count-- ) 
					((boolean[])retVal)[i++] = ResultSet.toBoolean( arrayContents[(int)index++] );
				break;
			case Types.SMALLINT:
			case Types.INTEGER:
				retVal = new int[ count ];
				for( ; count > 0; count-- ) 
					((int[])retVal)[i++] = ResultSet.toInt( arrayContents[(int)index++] );
				break;
			case Types.BIGINT:
				retVal = new long[ count ];
				for( ; count > 0; count-- )
					((long[])retVal)[i++] = ResultSet.toLong( arrayContents[(int)index++] );
				break;
			case Types.NUMERIC:
				retVal = new BigDecimal[ count ];
				for( ; count > 0; count-- ) 
					((BigDecimal[])retVal)[i] = ResultSet.toBigDecimal( arrayContents[(int)index++], 0 );
				break;
			case Types.REAL:
				retVal = new float[ count ];
				for( ; count > 0; count-- ) 
					((float[])retVal)[i++] = ResultSet.toFloat( arrayContents[(int)index++] );
				break;
			case Types.DOUBLE:
				retVal = new double[ count ];
				for( ; count > 0; count-- ) 
					((double[])retVal)[i++] = ResultSet.toDouble( arrayContents[(int)index++] );
				break;
			case Types.CHAR:
			case Types.VARCHAR:
				retVal = new String[ count ];
				for( ; count > 0; count-- ) 
					((String[])retVal)[i++] = arrayContents[(int)index++];
				break;
			case Types.DATE:
				retVal = new java.sql.Date[ count ];
				for( ; count > 0; count-- )
					((java.sql.Date[])retVal)[i++] = ResultSet.toDate( arrayContents[(int)index++] );
				break;
			case Types.TIME:
				retVal = new java.sql.Time[ count ];
				for( ; count > 0; count-- ) 
					((java.sql.Time[])retVal)[i++] = ResultSet.toTime( arrayContents[(int)index++] );
				break;
			case Types.TIMESTAMP:
				retVal = new Timestamp[ count ];
				StringBuffer sbuf = null;
				for( ; count > 0; count-- ) 
					((java.sql.Timestamp[])retVal)[i++] = ResultSet.toTimestamp( arrayContents[(int)index], rs );
				break;

			// Other datatypes not currently supported.  If you are really using other types ask
			// yourself if an array of non-trivial data types is really good database design.
			default:
      			throw org.postgresql.Driver.notImplemented();
		}
		return retVal;
	}

	public int getBaseType() throws SQLException {
		return Field.getSQLType( getBaseTypeName() );
	}

	public String getBaseTypeName() throws SQLException {
		String fType = field.getTypeName();
		if( fType.charAt(0) == '_' )
			fType = fType.substring(1);
		return fType;
	}

	public java.sql.ResultSet getResultSet() throws SQLException {
		return getResultSet( 1, 0, null );
	}

	public java.sql.ResultSet getResultSet(long index, int count) throws SQLException {
		return getResultSet( index, count, null );
	}

	public java.sql.ResultSet getResultSet(Map map) throws SQLException {
		return getResultSet( 1, 0, map );
	}

	public java.sql.ResultSet getResultSet(long index, int count, java.util.Map map) throws SQLException {
		Object array = getArray( index, count, map );
		Vector rows = new Vector();
		Field[] fields = new Field[2];
		fields[0] = new Field(conn, "INDEX", field.getOID("int2"), 2);
		switch ( getBaseType() )
		{
			case Types.BIT:
				boolean[] booleanArray = (boolean[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("bool"), 1);
				for( int i=0; i<booleanArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = (booleanArray[i]?"YES":"NO").getBytes(); // Value
					rows.addElement(tuple);
				}
			case Types.SMALLINT:
				fields[1] = new Field(conn, "VALUE", field.getOID("int2"), 2);
			case Types.INTEGER:
				int[] intArray = (int[]) array;
				if( fields[1] == null )
					fields[1] = new Field(conn, "VALUE", field.getOID("int4"), 4);
				for( int i=0; i<intArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = Integer.toString(intArray[i]).getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.BIGINT:
				long[] longArray = (long[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("int8"), 8);
				for( int i=0; i<longArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = Long.toString(longArray[i]).getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.NUMERIC:
				BigDecimal[] bdArray = (BigDecimal[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("numeric"), -1);
				for( int i=0; i<bdArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = bdArray[i].toString().getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.REAL:
				float[] floatArray = (float[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("float4"), 4);
				for( int i=0; i<floatArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = Float.toString(floatArray[i]).getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.DOUBLE:
				double[] doubleArray = (double[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("float8"), 8);
				for( int i=0; i<doubleArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = Double.toString(doubleArray[i]).getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.CHAR:
				fields[1] = new Field(conn, "VALUE", field.getOID("char"), 1);
			case Types.VARCHAR:
				String[] strArray = (String[]) array;
				if( fields[1] == null )
					fields[1] = new Field(conn, "VALUE", field.getOID("varchar"), -1);
				for( int i=0; i<strArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = strArray[i].getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.DATE:
				java.sql.Date[] dateArray = (java.sql.Date[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("date"), 4);
				for( int i=0; i<dateArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = dateArray[i].toString().getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.TIME:
				java.sql.Time[] timeArray = (java.sql.Time[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("time"), 8);
				for( int i=0; i<timeArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = timeArray[i].toString().getBytes(); // Value
					rows.addElement(tuple);
				}
				break;
			case Types.TIMESTAMP:
				java.sql.Timestamp[] timestampArray = (java.sql.Timestamp[]) array;
				fields[1] = new Field(conn, "VALUE", field.getOID("timestamp"), 8);
				for( int i=0; i<timestampArray.length; i++ ) {
					byte[][] tuple = new byte[2][0];
	  				tuple[0] = Integer.toString((int)index+i).getBytes(); // Index 
	  				tuple[1] = timestampArray[i].toString().getBytes(); // Value
					rows.addElement(tuple);
				}
				break;

			// Other datatypes not currently supported.  If you are really using other types ask
			// yourself if an array of non-trivial data types is really good database design.
			default:
      			throw org.postgresql.Driver.notImplemented();
		}
		return new ResultSet((org.postgresql.jdbc2.Connection)conn, fields, rows, "OK", 1 ); 
	}
}

