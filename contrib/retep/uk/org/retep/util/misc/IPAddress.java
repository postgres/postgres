package uk.org.retep.util.misc;

import java.util.StringTokenizer;

/**
 * Represent an IP address
 * @author
 * @version 1.0
 */

public class IPAddress
{
  protected long    address;
  protected long    b[] = new long[4];
  protected boolean invalid=true;

  public IPAddress()
  {
  }

  public IPAddress(String s)
  {
    setAddress(s);
  }

  public synchronized void setAddress(String s)
  {
    if(s==null || s.equals("")) {
      invalid=true;
      return;
    }

    address=0;
    StringTokenizer tok = new StringTokenizer(s,".");
    int i=0;
    while(i<4 && tok.hasMoreElements()) {
      b[i++] = Long.parseLong(tok.nextToken());
    }
    while(i<4) {
      b[i++]=0;
    }

    invalid=false;
    refresh();
  }

  public void refresh()
  {
    if(invalid)
      return;
    address = (b[0]<<24) | (b[1]<<16) | (b[2]<<8) | (b[3]);
  }

  public boolean isInvalid()
  {
    refresh();
    return invalid;
  }

  public String toString()
  {
    refresh();
    if(invalid)
      return "*INVALID*";

    return Long.toString(b[0])+"."+Long.toString(b[1])+"."+Long.toString(b[2])+"."+Long.toString(b[3]);
  }

  public boolean equals(Object o)
  {
    if(o instanceof IPAddress) {
      IPAddress ip = (IPAddress) o;

      refresh();
      ip.refresh();

      if(ip.invalid == invalid)
        return false;

      return address==ip.address;
    }
    return false;
  }

  private static int gethoststart(long b)
  {
    if((b & 0x80)==0x00) return 1; // class A
    if((b & 0xc0)==0x80) return 2; // class B
    if((b & 0xe0)==0xc0) return 3; // class C
    return 4;                      // class D
  }

  public boolean validateMask(IPAddress mask)
  {
    // If were a network check the host mask
    int i=gethoststart(b[0]);
System.out.println("Host start "+i);
    while(i<4 && b[i]==0) {
      if(mask.b[i++]>0)
        return false;
    }

    for(i=0;i<4;i++) {
      if((b[i]&mask.b[i])!=b[i])
        return false;
    }

    return true;
  }

  public IPAddress getMask()
  {
    IPAddress mask = new IPAddress();
    int i=3;
    while(i>-1 && b[i]==0) {
      mask.b[i--]=0;
    }
    while(i>-1) {
      mask.b[i--]=255;
    }
    mask.invalid=false;
    mask.refresh();
    return mask;
  }
}