package uk.org.retep.util.misc;

import java.io.*;
import java.util.Date;
import java.util.Iterator;
import java.util.Properties;
import java.util.TreeMap;

/**
 * Misc Properties utilities..
 * @author
 * @version 1.0
 */

public class PropertiesIO
{

  public PropertiesIO()
  {
  }

  /**
   * Builds a TreeMap based on the given Properties object. This is useful
   * because the keys will be in sorted order.
   */
  public static TreeMap getTreeMap(Properties p)
  {
    TreeMap map = new TreeMap();
    Iterator e = p.keySet().iterator();
    while(e.hasNext()) {
      Object k = e.next();
      map.put(k,p.get(k));
    }
    return map;
  }

  /**
   * Writes a Properties file to the writer. This is similar to Properties.save
   * except you can pick the key/value separator
   */
  public static synchronized void save(Properties p,OutputStream out,char sep,String header)
  throws IOException
  {
    save(p,p.keySet().iterator(),out,sep,header);
  }

  /**
   * Writes a Properties file to the writer. This is similar to Properties.save
   * except you can pick the key/value separator and the keys are written
   * in a sorted manner
   */
  public static synchronized void saveSorted(Properties p,OutputStream out,char sep,String header)
  throws IOException
  {
    save(p,getTreeMap(p).keySet().iterator(),out,sep,header);
  }

  /**
   * This is the same as save, only the keys in the enumeration are written.
   */
  public static synchronized void save(Properties p,Iterator e, OutputStream out,char sep,String header)
  throws IOException
  {
    BufferedWriter w = new BufferedWriter(new OutputStreamWriter(out, "8859_1"));

    if (header != null) {
      w.write('#');
      w.write(header);
      w.newLine();
    }

    w.write('#');
    w.write(new Date().toString());
    w.newLine();

    while(e.hasNext()) {
      String key = (String)e.next();
      w.write(encode(key,true));
      w.write(sep);
      w.write(encode((String)p.get(key),false));
      w.newLine();
    }
    w.flush();
  }

  private static final String specialSaveChars = "=: \t\r\n\f#!";

  /**
   * Encodes a string in a way similar to the JDK's Properties method
   */
  public static String encode(String s, boolean escapeSpace)
  {
    int l=s.length();
    StringBuffer buf = new StringBuffer(l<<1);

    for(int i=0;i<l;i++) {
      char c = s.charAt(i);
      switch(c)
        {
          case ' ':
            if(i==0 || escapeSpace) {
              buf.append('\\');
            }
            buf.append(' ');
            break;

          case '\\':
            buf.append('\\').append('\\');
            break;

          case '\t':
            buf.append('\\').append('t');
            break;

          case '\n':
            buf.append('\\').append('n');
            break;

          case '\r':
            buf.append('\\').append('r');
            break;

          case '\f':
            buf.append('\\').append('f');
            break;

          default:
            if((c<0x20)||(c>0x7e)) {
              buf.append('\\').append('u');
              buf.append(toHex((c >> 12) & 0xF));
              buf.append(toHex((c >>  8) & 0xF));
              buf.append(toHex((c >>  4) & 0xF));
              buf.append(toHex( c        & 0xF));
            } else {
              if (specialSaveChars.indexOf(c) != -1)
                buf.append('\\');
              buf.append(c);
            }
        }
    }
    return buf.toString();
  }

  /**
   * Convert a nibble to a hex character
   * @param	nibble	the nibble to convert.
   */
  public static char toHex(int n) {
    return hd[(n & 0xF)];
  }

  /** A table of hex digits */
  private static final char[] hd = {
    '0','1','2','3','4','5','6','7',
    '8','9','A','B','C','D','E','F'
  };
}