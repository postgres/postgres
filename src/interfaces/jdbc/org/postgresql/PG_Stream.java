package org.postgresql;

import java.io.*;
import java.lang.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import org.postgresql.*;
import org.postgresql.util.*;

/**
 * @version 1.0 15-APR-1997
 *
 * This class is used by Connection & PGlobj for communicating with the
 * backend.
 *
 * @see java.sql.Connection
 */
//  This class handles all the Streamed I/O for a org.postgresql connection
public class PG_Stream
{
  private Socket connection;
  private InputStream pg_input;
  private BufferedOutputStream pg_output;

  public PGStatement executingStatement;


  /**
   * Constructor:  Connect to the PostgreSQL back end and return
   * a stream connection.
   *
   * @param host the hostname to connect to
   * @param port the port number that the postmaster is sitting on
   * @exception IOException if an IOException occurs below it.
   */
  public PG_Stream(String host, int port) throws IOException
  {
    connection = new Socket(host, port);
    
    // Submitted by Jason Venner <jason@idiom.com> adds a 10x speed
    // improvement on FreeBSD machines (caused by a bug in their TCP Stack)
    connection.setTcpNoDelay(true);
    
    // Buffer sizes submitted by Sverre H Huseby <sverrehu@online.no>
    pg_input = new BufferedInputStream(connection.getInputStream(), 8192);
    pg_output = new BufferedOutputStream(connection.getOutputStream(), 8192);
  }
  
  /**
   * Set the currently executing statement. This is used to bind cached byte 
   * arrays to a Statement, so the statement can return the to the global 
   * pool of unused byte arrays when they are no longer inuse.
   */
  public void setExecutingStatement(PGStatement executingStatement){
      this.executingStatement = executingStatement;
  }
  
  /**
   * Sends a single character to the back end
   *
   * @param val the character to be sent
   * @exception IOException if an I/O error occurs
   */
  public void SendChar(int val) throws IOException
  {
      // Original code
      //byte b[] = new byte[1];
      //b[0] = (byte)val;
      //pg_output.write(b);
      
      // Optimised version by Sverre H. Huseby Aug 22 1999 Applied Sep 13 1999
      pg_output.write((byte)val);
  }
  
  /**
   * Sends an integer to the back end
   *
   * @param val the integer to be sent
   * @param siz the length of the integer in bytes (size of structure)
   * @exception IOException if an I/O error occurs
   */
  public void SendInteger(int val, int siz) throws IOException
  {
    byte[] buf = allocByteDim1(siz);
    
    while (siz-- > 0)
      {
	buf[siz] = (byte)(val & 0xff);
	val >>= 8;
      }
    Send(buf);
  }
  
  /**
   * Sends an integer to the back end in reverse order.
   *
   * This is required when the backend uses the routines in the
   * src/backend/libpq/pqcomprim.c module.
   *
   * As time goes by, this should become obsolete.
   *
   * @param val the integer to be sent
   * @param siz the length of the integer in bytes (size of structure)
   * @exception IOException if an I/O error occurs
   */
  public void SendIntegerReverse(int val, int siz) throws IOException
  {
    byte[] buf = allocByteDim1(siz);
    int p=0;
    while (siz-- > 0)
      {
	buf[p++] = (byte)(val & 0xff);
	val >>= 8;
      }
    Send(buf);
  }
  
  /**
   * Send an array of bytes to the backend
   *
   * @param buf The array of bytes to be sent
   * @exception IOException if an I/O error occurs
   */
  public void Send(byte buf[]) throws IOException
  {
    pg_output.write(buf);
  }
  
  /**
   * Send an exact array of bytes to the backend - if the length
   * has not been reached, send nulls until it has.
   *
   * @param buf the array of bytes to be sent
   * @param siz the number of bytes to be sent
   * @exception IOException if an I/O error occurs
   */
  public void Send(byte buf[], int siz) throws IOException
  {
    Send(buf,0,siz);
  }
  
  /**
   * Send an exact array of bytes to the backend - if the length
   * has not been reached, send nulls until it has.
   *
   * @param buf the array of bytes to be sent
   * @param off offset in the array to start sending from
   * @param siz the number of bytes to be sent
   * @exception IOException if an I/O error occurs
   */
  public void Send(byte buf[], int off, int siz) throws IOException
  {
    int i;
    
    pg_output.write(buf, off, ((buf.length-off) < siz ? (buf.length-off) : siz));
    if((buf.length-off) < siz)
      {
	for (i = buf.length-off ; i < siz ; ++i)
	  {
	    pg_output.write(0);
	  }
      }
  }
  
  /**
   * Sends a packet, prefixed with the packet's length
   * @param buf buffer to send
   * @exception SQLException if an I/O Error returns
   */
  public void SendPacket(byte[] buf) throws IOException
  {
    SendInteger(buf.length+4,4);
    Send(buf);
  }
  
  /**
   * Receives a single character from the backend
   *
   * @return the character received
   * @exception SQLException if an I/O Error returns
   */
  public int ReceiveChar() throws SQLException
  {
    int c = 0;
    
    try
      {
	c = pg_input.read();
	if (c < 0) throw new PSQLException("postgresql.stream.eof");
      } catch (IOException e) {
	throw new PSQLException("postgresql.stream.ioerror",e);
      }
      return c;
  }
  
  /**
   * Receives an integer from the backend
   *
   * @param siz length of the integer in bytes
   * @return the integer received from the backend
   * @exception SQLException if an I/O error occurs
   */
  public int ReceiveInteger(int siz) throws SQLException
  {
    int n = 0;
    
    try
      {
	for (int i = 0 ; i < siz ; i++)
	  {
	    int b = pg_input.read();
	    
	    if (b < 0)
	      throw new PSQLException("postgresql.stream.eof");
	    n = n | (b << (8 * i)) ;
	  }
      } catch (IOException e) {
	throw new PSQLException("postgresql.stream.ioerror",e);
      }
      return n;
  }
  
  /**
   * Receives an integer from the backend
   *
   * @param siz length of the integer in bytes
   * @return the integer received from the backend
   * @exception SQLException if an I/O error occurs
   */
  public int ReceiveIntegerR(int siz) throws SQLException
  {
    int n = 0;
    
    try
      {
	for (int i = 0 ; i < siz ; i++)
	  {
	    int b = pg_input.read();
	    
	    if (b < 0)
	      throw new PSQLException("postgresql.stream.eof");
	    n = b | (n << 8);
	  }
      } catch (IOException e) {
	throw new PSQLException("postgresql.stream.ioerror",e);
      }
      return n;
  }


  /**
   * Receives a null-terminated string from the backend.  Maximum of
   * maxsiz bytes - if we don't see a null, then we assume something
   * has gone wrong.
   *
   * @param maxsiz maximum length of string
   * @return string from back end
   * @exception SQLException if an I/O error occurs
   */
  public String ReceiveString(int maxsiz) throws SQLException
  {
    return ReceiveString(maxsiz, null);
  }

  /**
   * Receives a null-terminated string from the backend.  Maximum of
   * maxsiz bytes - if we don't see a null, then we assume something
   * has gone wrong.
   *
   * @param maxsiz maximum length of string
   * @param encoding the charset encoding to use.
   * @return string from back end
   * @exception SQLException if an I/O error occurs
   */
  public String ReceiveString(int maxsiz, String encoding) throws SQLException
  {
    byte[] rst = allocByteDim1(maxsiz);
    return ReceiveString(rst, maxsiz, encoding);
  }
  
  /**
   * Receives a null-terminated string from the backend.  Maximum of
   * maxsiz bytes - if we don't see a null, then we assume something
   * has gone wrong.
   *
   * @param rst byte array to read the String into. rst.length must 
   *        equal to or greater than maxsize. 
   * @param maxsiz maximum length of string in bytes
   * @param encoding the charset encoding to use.
   * @return string from back end
   * @exception SQLException if an I/O error occurs
   */
  public String ReceiveString(byte rst[], int maxsiz, String encoding) 
      throws SQLException
  {
    int s = 0;
    
    try
      {
	while (s < maxsiz)
	  {
	    int c = pg_input.read();
	    if (c < 0)
	      throw new PSQLException("postgresql.stream.eof");
	    else if (c == 0) {
		rst[s] = 0;
		break;
	    } else
	      rst[s++] = (byte)c;
	  }
	if (s >= maxsiz)
	  throw new PSQLException("postgresql.stream.toomuch");
      } catch (IOException e) {
	throw new PSQLException("postgresql.stream.ioerror",e);
      }
      String v = null;
      if (encoding == null)
          v = new String(rst, 0, s);
      else {
          try {
              v = new String(rst, 0, s, encoding);
          } catch (UnsupportedEncodingException unse) {
              throw new PSQLException("postgresql.stream.encoding", unse);
          }
      }
      return v;
  }
  
  /**
   * Read a tuple from the back end.  A tuple is a two dimensional
   * array of bytes
   *
   * @param nf the number of fields expected
   * @param bin true if the tuple is a binary tuple
   * @return null if the current response has no more tuples, otherwise
   *	an array of strings
   * @exception SQLException if a data I/O error occurs
   */
  public byte[][] ReceiveTuple(int nf, boolean bin) throws SQLException
  {
    int i, bim = (nf + 7)/8;
    byte[] bitmask = Receive(bim);
    byte[][] answer = allocByteDim2(nf);
    
    int whichbit = 0x80;
    int whichbyte = 0;
    
    for (i = 0 ; i < nf ; ++i)
      {
	boolean isNull = ((bitmask[whichbyte] & whichbit) == 0);
	whichbit >>= 1;
	if (whichbit == 0)
	  {
	    ++whichbyte;
	    whichbit = 0x80;
	  }
	if (isNull) 
	  answer[i] = null;
	else
	  {
	    int len = ReceiveIntegerR(4);
	    if (!bin) 
	      len -= 4;
	    if (len < 0) 
	      len = 0;
	    answer[i] = Receive(len);
	  }
      }
    return answer;
  }
  
  /**
   * Reads in a given number of bytes from the backend
   *
   * @param siz number of bytes to read
   * @return array of bytes received
   * @exception SQLException if a data I/O error occurs
   */
  private byte[] Receive(int siz) throws SQLException
  {
    byte[] answer = allocByteDim1(siz);
    Receive(answer,0,siz);
    return answer;
  }
  
  /**
   * Reads in a given number of bytes from the backend
   *
   * @param buf buffer to store result
   * @param off offset in buffer
   * @param siz number of bytes to read
   * @exception SQLException if a data I/O error occurs
   */
  public void Receive(byte[] b,int off,int siz) throws SQLException
  {
    int s = 0;
    
    try 
      {
	while (s < siz)
	  {
	    int w = pg_input.read(b, off+s, siz - s);
	    if (w < 0)
	      throw new PSQLException("postgresql.stream.eof");
	    s += w;
	  }
      } catch (IOException e) {
	  throw new PSQLException("postgresql.stream.ioerror",e);
      }
  }
  
  /**
   * This flushes any pending output to the backend. It is used primarily
   * by the Fastpath code.
   * @exception SQLException if an I/O error occurs
   */
  public void flush() throws SQLException
  {
    try {
      pg_output.flush();
    } catch (IOException e) {
      throw new PSQLException("postgresql.stream.flush",e);
    }
  }
  
  /**
   * Closes the connection
   *
   * @exception IOException if a IO Error occurs
   */
  public void close() throws IOException
  {
    pg_output.write("X\0".getBytes());
    pg_output.flush();
    pg_output.close();
    pg_input.close();
    connection.close();
  }

  /**
   * Deallocate all resources that has been associated with any previous
   * query.
   */
  public void deallocate(PGStatement stmt){

      for(int i = 0; i < maxsize_dim1; i++){
	  synchronized(notusemap_dim1[i]){
	      notusemap_dim1[i].addAll(stmt.inusemap_dim1[i]);
	  }
	  stmt.inusemap_dim1[i].clear();
      }

      for(int i = 0; i < maxsize_dim2; i++){
	  synchronized(notusemap_dim2[i]){
	      notusemap_dim2[i].addAll(stmt.inusemap_dim2[i]);
	  }
	  stmt.inusemap_dim2[i].clear();
      }
  }

  public static final int maxsize_dim1 = 256;
  public static ObjectPool notusemap_dim1[] = new ObjectPool[maxsize_dim1]; 
  public static byte binit[][] = new byte[maxsize_dim1][0];
  public static final int maxsize_dim2 = 32;
  public static ObjectPool notusemap_dim2[] = new ObjectPool[maxsize_dim2]; 
  public static ObjectPoolFactory factory_dim1;
  public static ObjectPoolFactory factory_dim2;

  static {
      for(int i = 0; i < maxsize_dim1; i++){
	  binit[i] = new byte[i];
	  notusemap_dim1[i] = new ObjectPool();
      }
      for(int i = 0; i < maxsize_dim2; i++){
	  notusemap_dim2[i] = new ObjectPool();
      }
      factory_dim1 = ObjectPoolFactory.getInstance(maxsize_dim1);
      factory_dim2 = ObjectPoolFactory.getInstance(maxsize_dim2);
  }

  public byte[] allocByteDim1(int size){
      if(size >= maxsize_dim1 || executingStatement == null){
	  return new byte[size];
      }
      ObjectPool not_usel = notusemap_dim1[size];
      ObjectPool in_usel = executingStatement.inusemap_dim1[size];

      byte b[] = null;

      synchronized(not_usel){
	  if(!not_usel.isEmpty()) {
	      Object o = not_usel.remove();
	      b = (byte[]) o;
	  } else {
	      b = new byte[size];
	  }
      }
      in_usel.add(b);

      return b;
  }    

  public byte[][] allocByteDim2(int size){
      if(size >= maxsize_dim2 || executingStatement == null){
	  return new byte[size][0];
      }
      ObjectPool not_usel = notusemap_dim2[size];
      ObjectPool in_usel = executingStatement.inusemap_dim2[size];

      byte b[][] = null;
      
      synchronized(not_usel){
	  if(!not_usel.isEmpty()) {
	      Object o = not_usel.remove();
	      b = (byte[][]) o;
	  } else 
	      b = new byte[size][0];
	  
	  in_usel.add(b);
      }

      return b;
  }    

}





