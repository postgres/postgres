/**
 *
 * This implements a circle consisting of a point and a radius
 *
 */

package postgresql;

import java.io.*;
import java.sql.*;

public class PGcircle implements Serializable
{
  /**
   * This is the centre point
   */
  public PGpoint center;
  
  /**
   * This is the radius
   */
  double radius;
  
  public PGcircle(double x,double y,double r)
  {
    this.center = new PGpoint(x,y);
    this.radius = r;
  }
  
  public PGcircle(PGpoint c,double r)
  {
    this.center = c;
    this.radius = r;
  }
  
  public PGcircle(PGcircle c)
  {
    this.center = new PGpoint(c.center);
    this.radius = c.radius;
  }
  
  /**
   * This constructor is used by the driver.
   */
  public PGcircle(String s) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(PGtokenizer.removeAngle(s),',');
    if(t.getSize() != 2)
      throw new SQLException("conversion of circle failed - "+s);
    
    try {
      center = new PGpoint(t.getToken(0));
      radius = Double.valueOf(t.getToken(1)).doubleValue();
    } catch(NumberFormatException e) {
      throw new SQLException("conversion of circle failed - "+s+" - +"+e.toString());
    }
  }
  
  public boolean equals(Object obj)
  {
    PGcircle p = (PGcircle)obj;
    return p.center.equals(center) && p.radius==radius;
  }
  
  /**
   * This returns the circle in the syntax expected by postgresql
   */
  public String toString()
  {
    return "<"+center+","+radius+">";
  }
}
