package postgresql.util;

import java.io.*;
import java.sql.*;

/**
 * This implements a class that handles the PostgreSQL money and cash types
 */
public class PGmoney extends PGobject implements Serializable,Cloneable
{
  /**
   * The value of the field
   */
  public double val;
  
  /**
   * @param value of field
   */
  public PGmoney(double value) {
    this();
    val = value;
  }
  
  /**
   * This is called mainly from the other geometric types, when a
   * point is imbeded within their definition.
   *
   * @param value Definition of this point in PostgreSQL's syntax
   */
  public PGmoney(String value) throws SQLException
  {
    this();
    setValue(value);
  }
  
  /**
   * Required by the driver
   */
  public PGmoney()
  {
    setType("money");
  }
  
  /**
   * @param s Definition of this point in PostgreSQL's syntax
   * @exception SQLException on conversion failure
   */
  public void setValue(String s) throws SQLException
  {
    try {
      val = Double.valueOf(s.substring(1)).doubleValue();
    } catch(NumberFormatException e) {
      throw new SQLException("conversion of money failed - "+e.toString());
    }
  }
  
  /**
   * @param obj Object to compare with
   * @return true if the two boxes are identical
   */
  public boolean equals(Object obj)
  {
    if(obj instanceof PGmoney) {
      PGmoney p = (PGmoney)obj;
      return val == p.val;
    }
    return false;
  }
  
  /**
   * This must be overidden to allow the object to be cloned
   */
  public Object clone()
  {
    return new PGmoney(val);
  }
  
  /**
   * @return the PGpoint in the syntax expected by postgresql
   */
  public String getValue()
  {
    return "$"+val;
  }
}
