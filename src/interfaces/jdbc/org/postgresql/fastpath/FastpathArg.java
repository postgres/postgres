package org.postgresql.fastpath;

import java.io.*;
import java.lang.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import org.postgresql.util.*;

/**
 * Each fastpath call requires an array of arguments, the number and type
 * dependent on the function being called.
 *
 * <p>This class implements methods needed to provide this capability.
 *
 * <p>For an example on how to use this, refer to the org.postgresql.largeobject
 * package
 *
 * @see org.postgresql.fastpath.Fastpath
 * @see org.postgresql.largeobject.LargeObjectManager
 * @see org.postgresql.largeobject.LargeObject
 */
public class FastpathArg
{
  /**
   * Type of argument, true=integer, false=byte[]
   */
  public boolean type;
  
  /**
   * Integer value if type=true
   */
  public int value;
  
  /**
   * Byte value if type=false;
   */
  public byte[] bytes;
  
  /**
   * Constructs an argument that consists of an integer value
   * @param value int value to set
   */
  public FastpathArg(int value)
  {
    type=true;
    this.value=value;
  }
  
  /**
   * Constructs an argument that consists of an array of bytes
   * @param bytes array to store
   */
  public FastpathArg(byte bytes[])
  {
    type=false;
    this.bytes=bytes;
  }
  
  /**
   * Constructs an argument that consists of part of a byte array
   * @param buf source array
   * @param off offset within array
   * @param len length of data to include
   */
  public FastpathArg(byte buf[],int off,int len)
  {
    type=false;
    bytes = new byte[len];
    System.arraycopy(buf,off,bytes,0,len);
  }
  
  /**
   * Constructs an argument that consists of a String.
   * @param s String to store
   */
  public FastpathArg(String s)
  {
    this(s.getBytes());
  }
  
  /**
   * This sends this argument down the network stream.
   *
   * <p>The stream sent consists of the length.int4 then the contents.
   *
   * <p><b>Note:</b> This is called from Fastpath, and cannot be called from
   * client code.
   *
   * @param s output stream
   * @exception IOException if something failed on the network stream
   */
  protected void send(org.postgresql.PG_Stream s) throws IOException
  {
    if(type) {
      // argument is an integer
      s.SendInteger(4,4);	// size of an integer
      s.SendInteger(value,4);	// integer value of argument
    } else {
      // argument is a byte array
      s.SendInteger(bytes.length,4);	// size of array
      s.Send(bytes);
    }
  }
}

