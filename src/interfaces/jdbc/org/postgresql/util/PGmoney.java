package org.postgresql.util;

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
      String s1;
      boolean negative;

      negative = (s.charAt(0) == '(') ;

      // Remove any () (for negative) & currency symbol
      s1 = PGtokenizer.removePara(s).substring(1);

      // Strip out any , in currency
      int pos = s1.indexOf(',');
      while (pos != -1) {
        s1 = s1.substring(0,pos) + s1.substring(pos +1);
        pos = s1.indexOf(',');
      }

      val = Double.valueOf(s1).doubleValue();
      val = negative ? -val : val;

    } catch(NumberFormatException e) {
      throw new PSQLException("postgresql.money",e);
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
   * @return the PGpoint in the syntax expected by org.postgresql
   */
  public String getValue()
  {
    if (val < 0) {
      return "-$" + (-val);
    }
    else {
      return "$"+val;
    }
  }
}
