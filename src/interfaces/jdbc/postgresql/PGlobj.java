// Java Interface to Postgres
// $Id: PGlobj.java,v 1.1 1997/09/20 02:21:22 scrappy Exp $

// Copyright (c) 1997 Peter T Mount

package postgresql;

import java.sql.*;
import java.math.*;
import java.net.*;
import java.io.*;
import java.util.*;

/**
 * This class implements the large object interface to postgresql.
 *
 * It provides the basic methods required to run the interface, plus
 * a pair of methods that provide InputStream and OutputStream classes
 * for this object.
 */
public class PGlobj
{
  // This table contains the function oid's used by the backend
  private Hashtable func = new Hashtable();
  
  protected postgresql.Connection conn;
  
  /**
   * These are the values for mode, taken from libpq-fs.h
   */
  public static final int INV_ARCHIVE = 0x00010000;
  public static final int INV_WRITE   = 0x00020000;
  public static final int INV_READ    = 0x00040000;
  
  /**
   * These are the functions that implement the interface
   */
  private static final String OPEN	= "lo_open";
  private static final String CLOSE	= "lo_close";
  private static final String CREATE	= "lo_creat";
  private static final String UNLINK	= "lo_unlink";
  private static final String SEEK	= "lo_lseek";
  private static final String TELL	= "lo_tell";
  private static final String READ	= "loread";
  private static final String WRITE	= "lowrite";
  
  /**
   * This creates the interface
   */
  public PGlobj(Connection conn) throws SQLException
  {
    if(!(conn instanceof postgresql.Connection))
      throw new SQLException("PGlobj: Wrong connection class");
    
    this.conn = (postgresql.Connection)conn;
    ResultSet res = (postgresql.ResultSet)conn.createStatement().executeQuery("select proname, oid from pg_proc" +
				      " where proname = 'lo_open'" +
				      "    or proname = 'lo_close'" +
				      "    or proname = 'lo_creat'" +
				      "    or proname = 'lo_unlink'" +
				      "    or proname = 'lo_lseek'" +
				      "    or proname = 'lo_tell'" +
				      "    or proname = 'loread'" +
				      "    or proname = 'lowrite'");
    
    if(res==null)
      throw new SQLException("failed to initialise large object interface");
    
    while(res.next()) {
      func.put(res.getString(1),new Integer(res.getInt(2)));
      DriverManager.println("PGlobj:func "+res.getString(1)+" oid="+res.getInt(2));
    }
    res.close();
  }
  
  // this returns the oid of the function
  private int getFunc(String name) throws SQLException
  {
    Integer i = (Integer)func.get(name);
    if(i==null)
      throw new SQLException("unknown function: "+name);
    return i.intValue();
  }
  
  /**
   * This calls a function on the backend
   * @param fnid oid of the function to run
   * @param args array containing args, 3 ints per arg
   */
  public int PQfn(int fnid,int args[]) throws SQLException
  {
    return PQfn(fnid,args,null,0,0);
  }
  
  // fix bug in 6.1.1
  public void writeInt(DataOutputStream data,int i) throws IOException
  {
    data.writeByte((i>>24)&0xff);
    data.writeByte( i     &0xff);
    data.writeByte((i>>8) &0xff);
    data.writeByte((i>>16)&0xff);
  }
  
  /**
   * This calls a function on the backend
   * @param fnid oid of the function to run
   * @param args array containing args, 3 ints per arg
   * @param buf byte array to write into, null returns result as an integer
   * @param off offset in array
   * @param len number of bytes to read
   */
  public int PQfn(int fnid,int args[],byte buf[],int off,int len) throws SQLException
  {
    //ByteArrayOutputStream b = new ByteArrayOutputStream();
    //DataOutputStream data = new DataOutputStream(b);
    int in = -1;
    
    try {
      int al=args.length/3;
      
      // For some reason, the backend takes these in the reverse order
      byte b[] = new byte[2+4+4];
      int bp=0;
      b[bp++]='F';
      b[bp++]=0;
      b[bp++]=(byte)((fnid)&0xff);
      b[bp++]=(byte)((fnid>>24)&0xff);
      b[bp++]=(byte)((fnid>>16)&0xff);
      b[bp++]=(byte)((fnid>>8)&0xff);
      b[bp++]=(byte)((al)&0xff);
      b[bp++]=(byte)((al>>24)&0xff);
      b[bp++]=(byte)((al>>16)&0xff);
      b[bp++]=(byte)((al>>8)&0xff);
      conn.pg_stream.Send(b);
      
      //conn.pg_stream.SendChar('F');
      //conn.pg_stream.SendInteger(fnid,4);
      //conn.pg_stream.SendInteger(args.length / 3,4);
      
      int l = args.length-1;
      if(args[l]==0) l--;
      
      for(int i=0;i<l;i++)
	conn.pg_stream.SendInteger(args[i],4);
      
      if(args[args.length-1]==0)
	conn.pg_stream.Send(buf,off,len);
      
    } catch(Exception e) {
      throw new SQLException("lo_open failed");
    }
    //try {
      if((in = conn.pg_stream.ReceiveChar())!='V') {
	if(in=='E')
	  throw new SQLException(conn.pg_stream.ReceiveString(4096));
	throw new SQLException("lobj: expected 'V' from backend, got "+((char)in));
      }
      
      while(true) {
	in = conn.pg_stream.ReceiveChar();
	switch(in)
	  {
	  case 'G':
	    if(buf==null)
	      in = conn.pg_stream.ReceiveInteger(4);
	    else
	      conn.pg_stream.Receive(buf,off,len);
	    conn.pg_stream.ReceiveChar();
	    return in;
	    
	  case 'E':
	    throw new SQLException("lobj: error - "+conn.pg_stream.ReceiveString(4096));
	    
	  case 'N':
	    conn.pg_stream.ReceiveString(4096);
	    break;
	    
	  case '0':
	    return -1;
	    
	  default:
	    throw new SQLException("lobj: protocol error");
	  }
      }
      //    } catch(IOException ioe) {
      //      throw new SQLException("lobj: Network error - "+ioe);
      //}
  }
  
  /**
   * This opens a large object. It returns a handle that is used to
   * access the object.
   */
  public int open(int lobjId,int mode) throws SQLException
  {
    int args[] = new int[2*3];
    args[0] = args[3] = 4;
    args[1] = args[4] = 1;
    args[2] = lobjId;
    args[5] = mode;
    
    int fd = PQfn(getFunc(OPEN),args);
    if(fd<0)
      throw new SQLException("lo_open: no object");
    seek(fd,0);
    return fd;
  }
  
  /**
   * This closes a large object.
   */
  public void close(int fd) throws SQLException
  {
    int args[] = new int[1*3];
    args[0] = 4;
    args[1] = 1;
    args[2] = fd;
    
    // flush/close streams here?
    PQfn(getFunc(CLOSE),args);
  }
  
  /**
   * This reads a block of bytes from the large object
   * @param fd descriptor for an open large object
   * @param buf byte array to write into
   * @param off offset in array
   * @param len number of bytes to read
   */
  public void read(int fd,byte buf[],int off,int len) throws SQLException
  {
    int args[] = new int[2*3];
    args[0] = args[3] = 4;
    args[1] = args[4] = 1;
    args[2] = fd;
    args[5] = len;
    
    PQfn(getFunc(READ),args,buf,off,len);
  }
  
  /**
   * This writes a block of bytes to an open large object
   * @param fd descriptor for an open large object
   * @param buf byte array to write into
   * @param off offset in array
   * @param len number of bytes to read
   */
  public void write(int fd,byte buf[],int off,int len) throws SQLException
  {
    int args[] = new int[2*3];
    args[0] = args[3] = 4;
    args[1] = args[4] = 1;
    args[2] = fd;
    args[5] = 0;
    
    PQfn(getFunc(WRITE),args,buf,off,len);
  }
  
  /**
   * This sets the current read or write location on a large object.
   * @param fd descriptor of an open large object
   * @param off offset in object
   */
  public void seek(int fd,int off) throws SQLException
  {
    int args[] = new int[3*3];
    args[0] = args[3] = args[6] = 4;
    args[1] = args[4] = args[7] = 1;
    args[2] = fd;
    args[5] = off;
    args[8] = 0;	// SEEK
    
    PQfn(getFunc(SEEK),args);
  }
  
  /**
   * This creates a new large object.
   *
   * the mode is a bitmask describing different attributes of the new object
   *
   * returns the oid of the large object created.
   */
  public int create(int mode) throws SQLException
  {
    int args[] = new int[1*3];
    args[0] = 4;
    args[1] = 1;
    args[2] = mode;
    
    return PQfn(getFunc(CREATE),args);
  }
  
  /**
   * This returns the current location within the large object
   */
  public int tell(int fd) throws SQLException
  {
    int args[] = new int[1*3];
    args[0] = 4;
    args[1] = 1;
    args[2] = fd;
    
    return PQfn(getFunc(TELL),args);
  }
  
  /**
   * This removes a large object from the database
   */
  public void unlink(int fd) throws SQLException
  {
    int args[] = new int[1*3];
    args[0] = 4;
    args[1] = 1;
    args[2] = fd;
    
    PQfn(getFunc(UNLINK),args);
  }
  
  /**
   * This returns an InputStream based on an object
   */
  public InputStream getInputStream(int fd) throws SQLException
  {
    return (InputStream) new PGlobjInput(this,fd);
  }
  
  /**
   * This returns an OutputStream based on an object
   */
  public OutputStream getOutputStream(int fd) throws SQLException
  {
    return (OutputStream) new PGlobjOutput(this,fd);
  }
  
  /**
   * As yet, the lo_import and lo_export functions are not implemented.
   */
}

// This class implements an InputStream based on a large object
//
// Note: Unlike most InputStreams, this one supports mark()/reset()
//
class PGlobjInput extends InputStream
{
  private PGlobj obj;
  private int	 fd;
  
  private int	mp;	// mark position
  private int	rl;	// read limit
  
  // This creates an Input stream based for a large object
  public PGlobjInput(PGlobj obj,int fd)
  {
    this.obj = obj;
    this.fd = fd;
  }
  
  public int read() throws IOException
  {
    byte b[] = new byte[1];
    read(b,0,1);
    return (int)b[0];
  }
  
  public int read(byte b[],int off,int len) throws IOException
  {
    try {
      obj.read(fd,b,off,len);
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
    return len;
  }
  
  public long skip(long n) throws IOException
  {
    try {
      int cp = obj.tell(fd);
      obj.seek(fd,cp+(int)n);
      return obj.tell(fd) - cp;
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
  }
  
  public synchronized void mark(int readLimit)
  {
    try {
      mp = obj.tell(fd);
      rl = readLimit;
    } catch(SQLException e) {
      // We should throw an exception here, but mark() doesn't ;-(
    }
  }
  
  public void reset() throws IOException
  {
    try {
      int cp = obj.tell(fd);
      if((cp-mp)>rl)
	throw new IOException("mark invalidated");
      obj.seek(fd,mp);
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
  }
  
  public boolean markSupported()
  {
    return true;
  }
  
  
  public void close() throws IOException
  {
    try {
      obj.close(fd);
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
  }
}

// This class implements an OutputStream to a large object
class PGlobjOutput extends OutputStream
{
  private PGlobj obj;
  private int	 fd;
  
  // This creates an Input stream based for a large object
  public PGlobjOutput(PGlobj obj,int fd)
  {
    this.obj = obj;
    this.fd = fd;
  }
  
  public void write(int i) throws IOException
  {
    byte b[] = new byte[1];
    b[0] = (byte)i;
    write(b,0,1);
  }
  
  public void write(byte b[],int off,int len) throws IOException
  {
    try {
      obj.write(fd,b,off,len);
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
  }
  
  public void close() throws IOException
  {
    try {
      obj.close(fd);
    } catch(SQLException e) {
      throw new IOException(e.toString());
    }
  }
}
