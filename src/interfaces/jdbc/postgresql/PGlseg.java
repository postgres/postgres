/**
 *
 * This implements a lseg (line segment) consisting of two points
 *
 */

package postgresql;

import java.io.*;
import java.sql.*;

public class PGlseg implements Serializable
{
  /**
   * These are the two points.
   */
  public PGpoint point[] = new PGpoint[2];
  
  public PGlseg(double x1,double y1,double x2,double y2)
  {
    this.point[0] = new PGpoint(x1,y1);
    this.point[1] = new PGpoint(x2,y2);
  }
  
  public PGlseg(PGpoint p1,PGpoint p2)
  {
    this.point[0] = p1;
    this.point[1] = p2;
  }
  
  /**
   * This constructor is used by the driver.
   */
  public PGlseg(String s) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(PGtokenizer.removeBox(s),',');
    if(t.getSize() != 2)
      throw new SQLException("conversion of lseg failed - "+s);
    
    point[0] = new PGpoint(t.getToken(0));
    point[1] = new PGpoint(t.getToken(1));
  }
  
  public boolean equals(Object obj)
  {
    PGlseg p = (PGlseg)obj;
    return (p.point[0].equals(point[0]) && p.point[1].equals(point[1])) ||
      (p.point[0].equals(point[1]) && p.point[1].equals(point[0]));
  }
  
  /**
   * This returns the lseg in the syntax expected by postgresql
   */
  public String toString()
  {
    return "["+point[0]+","+point[1]+"]";
  }
}
