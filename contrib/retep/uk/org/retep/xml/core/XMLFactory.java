package uk.org.retep.xml.core;

import java.io.IOException;
import java.io.Writer;

/**
 * An XMLFactory is used to render XML Tags, accounting for nesting etc
 */
public class XMLFactory
{
  /**
   * The lest level (ie, how many tags down the tree we are)
   */
  protected int level;

  /**
   * The size of our tag name cache
   */
  protected int maxlevel;

  /**
   * Our tag name cache
   */
  protected String[] names;

  /**
   * Used to keep track of how formatting is done
   */
  protected boolean hascontent;
  protected boolean[] contbuf;

  /**
   * Scratch used by nest()
   */
  private char[] nestbuf;

  /**
   * The destination Writer
   */
  protected Writer out;

  /**
   * True if we are still within a tag
   */
  protected boolean inTag;

  /**
   * True if we have just created a tag so parameters are valid
   */
  protected boolean inArg;

  /**
   * Constructs an XMLFactory with no output Writer
   */
  public XMLFactory()
  {
    this(10);
  }

  /**
   * Constructs an XMLFactory with no output Writer
   * @param m Expected number of leaves in the XML Tree
   */
  public XMLFactory(int m)
  {
    // Initialise the names cache
    level=0;
    maxlevel=m;
    names=new String[maxlevel];
    contbuf=new boolean[maxlevel];

    // This is used by nest()
    nestbuf=new char[maxlevel];
    for(int i=0;i<maxlevel;i++)
      nestbuf[i]=' ';
  }

  /**
   * Constructs an XMLFactory
   * @param out Writer to send the output to
   */
  public XMLFactory(Writer out)
  throws IOException
  {
    this();
    setWriter(out);
  }

  /**
   * Constructs an XMLFactory
   * @param out Writer to send the output to
   * @param encoding The XML encoding
   */
  public XMLFactory(Writer out,String encoding)
  throws IOException
  {
    this();
    setWriter(out,encoding);
  }

  /**
   * Constructs an XMLFactory
   * @param out Writer to send the output to
   * @param m Expected number of leaves in the XML Tree
   */
  public XMLFactory(int m,Writer out)
  throws IOException
  {
    this(m);
    setWriter(out);
  }

  /**
   * Constructs an XMLFactory
   * @param out Writer to send the output to
   * @param encoding The XML encoding
   * @param m Expected number of leaves in the XML Tree
   */
  public XMLFactory(int m,Writer out,String encoding)
  throws IOException
  {
    this(m);
    setWriter(out,encoding);
  }

  /**
   * Sets the Writer to send the output to. This call will also send the
   * XML header.
   *
   * @param out Writer to send output to
   */
  public void setWriter(Writer out)
  throws IOException
  {
    setWriter(out,"ISO-8859-1");
  }

  /**
   * Sets the Writer to send the output to. This call will also send the
   * XML header using the supplied encoding. It is up to the user code to
   * implement this encoding.
   *
   * @param out Writer to send output to
   * @param encoding Encoding of the XML Output
   */
  public void setWriter(Writer out,String encoding)
  throws IOException
  {
    this.out=out;
    out.write("<?xml version=\"1.0\" encoding=\"");
    out.write(encoding);
    out.write("\" ?>\n");
  }

  /**
   * @return Writer the XML is being sent out on.
   */
  public Writer getWriter() {
    return out;
  }

  /**
   * This starts a tag
   * @param name The tag name
   */
  public void startTag(String name)
  throws IOException
  {
    if(inTag && inArg) {
      // Handles two startTag() calls in succession.
      out.write(">");
    }

    nest(level);
    out.write('<');
    out.write(name);
    inTag=true;
    inArg=true;

    // cache the current tag name
    names[level]=name;

    // cache the current hascontent value & reset
    contbuf[level]=hascontent;
    hascontent=false;

    // increase the level and the cache's as necessary
    level++;
    if(level>maxlevel) {
      maxlevel=maxlevel+10;

      String n[]=new String[maxlevel];
      System.arraycopy(names,0,n,0,level);
      names=n;

      boolean b[] = new boolean[maxlevel];
      System.arraycopy(contbuf,0,b,0,level);
      contbuf=b;
    }
  }

  /**
   * This ends a tag
   */
  public void endTag()
  throws IOException, XMLFactoryException
  {
    if(level<1)
      throw new XMLFactoryException("endTag called above root node");

    level--;

    if(inArg) {
      // We are still within the opening tag
      out.write(" />");
    } else {
      // We must have written some content or child tags

      // hascontent is true if addContent() was called. If it was never called
      // to get here some child tags must have been written, so we call nest()
      // so that the close tag is on it's own line, and everything looks neat
      // and tidy.
      if(!hascontent)
        nest(level);

      out.write("</");
      out.write(names[level]);
      out.write('>');
    }

    inArg=false;    // The parent tag must be told it now has content
    inTag= level>0; // Are we still in a tag?
    hascontent=contbuf[level];  // retrieve this level's hascontent value
  }

  /**
   * This completes the document releasing any open resources.
   */
  public void close()
  throws IOException, XMLFactoryException
  {
    while(level>0)
      endTag();
    out.write('\n');
    out.flush();
  }

  /**
   * This writes an attribute to the current tag. If the value is null, then no action is taken.
   * @param name Name of the parameter
   * @param value Value of the parameter
   * @throw XMLFactoryException if out of context
   */
  public void addAttribute(String name,Object value)
  throws IOException, XMLFactoryException
  {
    if(value==null)
      return;

    if(inArg) {
      out.write(' ');
      out.write(name);
      out.write("=\"");
      out.write(encode(value.toString()));
      out.write("\"");
    } else
      throw new XMLFactoryException("Cannot add attribute outside of a tag");
  }

  /**
   * This writes some content to the current tag. Once this has been called,
   * you cannot add any more attributes to the current tag. Note, if c is null,
   * no action is taken.
   * @param c content to add.
   */
  public void addContent(Object c)
  throws IOException, XMLFactoryException
  {
    if(c==null)
      return;

    if(inTag) {
      if(inArg) {
        // close the open tag
        out.write('>');
        inArg=false;
      }
      out.write(c.toString());

      // This is used by endTag()
      hascontent=true;
    } else
      throw new XMLFactoryException("Cannot add content outside of a tag");
  }

  /**
   * This adds a comment to the XML file. This is normally used at the start of
   * any XML output.
   * @parm c Comment to include
   */
  public void addComment(Object c)
  throws IOException, XMLFactoryException
  {
    if(inTag)
      throw new XMLFactoryException("Cannot add comments within a tag");

    out.write("\n<!-- ");
    out.write(c.toString());
    out.write(" -->");
  }

  /**
   * Indents the output according to the level
   * @param level The indent level to generate
   */
  protected void nest(int level)
  throws IOException
  {
    out.write('\n');
    while(level>nestbuf.length) {
      out.write(nestbuf,0,nestbuf.length);
      level-=nestbuf.length;
    }
    out.write(nestbuf,0,level);
  }

  /**
   * Encodes the string so that any XML tag chars are translated
   */
  protected String encode(String s) {
    return s;
  }

}