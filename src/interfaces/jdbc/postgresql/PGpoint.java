/**
 *
 * This implements a version of java.awt.Point, except it uses double
 * to represent the coordinates.
 *
 * It maps to the point datatype in postgresql.
 */

package postgresql;

import java.awt.Point;
import java.io.*;
import java.sql.*;

public class PGpoint  implements Serializable
{
  /**
   * These are the coordinates.
   * These are public, because their equivalents in java.awt.Point are
   */
  public double x,y;
  
  public PGpoint(double x,double y)
  {
    this.x = x;
    this.y = y;
  }
  
  public PGpoint(PGpoint p)
  {
    this(p.x,p.y);
  }
  
  /**
   * This constructor is used by the driver.
   */
  public PGpoint(String s) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(PGtokenizer.removePara(s),',');
    try {
      x = Double.valueOf(t.getToken(0)).doubleValue();
      y = Double.valueOf(t.getToken(1)).doubleValue();
    } catch(NumberFormatException e) {
      throw new SQLException("conversion of point failed - "+e.toString());
    }
  }
  
  public boolean equals(Object obj)
  {
    PGpoint p = (PGpoint)obj;
    return x == p.x && y == p.y;
  }
  
  /**
   * This returns the point in the syntax expected by postgresql
   */
  public String toString()
  {
    return "("+x+","+y+")";
  }
  
  public void translate(int x,int y)
  {
    translate((double)x,(double)y);
  }
  
  public void translate(double x,double y)
  {
    this.x += x;
    this.y += y;
  }
  
  public void move(int x,int y)
  {
    setLocation(x,y);
  }
  
  public void move(double x,double y)
  {
    this.x = x;
    this.y = y;
  }
  
  // refer to java.awt.Point for description of this
  public void setLocation(int x,int y)
  {
    move((double)x,(double)y);
  }
  
  // refer to java.awt.Point for description of this
  public void setLocation(Point p)
  {
    setLocation(p.x,p.y);
  }
  
}
