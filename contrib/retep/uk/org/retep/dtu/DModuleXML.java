package uk.org.retep.dtu;

import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;
import uk.org.retep.xml.parser.TagHandler;
import uk.org.retep.xml.parser.TagListener;
import uk.org.retep.util.Logger;

import java.io.CharArrayWriter;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashMap;
import java.util.Iterator;
import java.util.ArrayList;

import org.xml.sax.InputSource;
import org.xml.sax.Parser;
import org.xml.sax.SAXException;
import javax.xml.parsers.ParserConfigurationException;

public class DModuleXML implements TagListener
{
  protected TagHandler handler;

  protected DModule module = null;
  protected DNode node = null;
  protected DTransform trans = null;

  protected ArrayList txmap;

  public DModuleXML()
  {
    handler = new TagHandler();
    handler.addTagListener(this);

    txmap = new ArrayList();

    Logger.log(Logger.DEBUG,"DModuleXML initialised");
  }

  public TagHandler getTagHandler()
  {
    return handler;
  }

  /**
   * Used to optimise the switch handling in tagStart.
   *
   * The values of each T_* constant must match the corresponding element no
   * in the tags static array.
   */
  private static final int T_DEFAULT=-1;
  private static final int T_MODULE =0;
  private static final int T_NODE   =1;
  private static final int T_TRANS  =2;
  private static final String tags[] = {
    DConstants.XML_MODULE,
    DConstants.XML_NODE,
    DConstants.XML_TRANSFORM
  };

  /**
   * This is called when a tag has just been started.
   * <p><b>NB:</b> args is volatile, so if you use it beyond the lifetime of
   * this call, then you must make a copy of the HashMap (and not use simply
   * store this HashMap).
   * @param level The number of tags above this
   * @param tag The tag name
   * @param args A HashMap of any arguments
   */
  public void tagStart(int level,String tag,HashMap args)
  {
    Logger.log(Logger.DEBUG,"DModuleXML.tagStart",tag);

    // Prefetch some common attributes
    String sType = (String) args.get(DConstants.XML_TYPE);
    String sX = (String) args.get(DConstants.XML_X);
    String sY = (String) args.get(DConstants.XML_Y);

    int type=-1,x=-1,y=-1;

    if(sType!=null) {
      type = Integer.parseInt(sType);
    }

    if(sX!=null) {
      y = Integer.parseInt(sX);
    }

    if(sY!=null) {
      x = Integer.parseInt(sY);
    }

    // Match the tag against the tags array (used for switch() )
    int tagID=T_DEFAULT;
    for(int i=0;i<tags.length;i++) {
      if(tag.equals(tags[i])) {
        tagID=i;
      }
    }

    switch(tagID)
      {
          // The main module tag
        case T_MODULE:
          module = new DModule();

          String sDisplayName = (String) args.get(DConstants.XML_DISPLAYNAME);
          if(sDisplayName!=null) {
            module.setDisplayName(sDisplayName);
          }
          break;

          // Basic nodes
        case T_NODE:
          node = new DNode();
          node.setType(type);
          module.addNode(node);
          break;

          // Basic transforms
        case T_TRANS:
          trans = new DTransform();
          trans.setType(type);

          // When finished we fix the transforms
          int to = Integer.parseInt((String) args.get(DConstants.XML_TO));
          txmap.add(new tx(node,trans,to));

          break;

        default:
          // ignore unknown tags for now
          break;
      }
  }

  protected class tx
  {
    public DNode node;
    public DTransform transform;
    public int toID;

    public tx(DNode aNode,DTransform aTransform,int aID)
    {
      node=aNode;
      transform=aTransform;
      toID=aID;
    }
  }

  /**
   * This method is called by ContHandler to process a tag once it has been
   * fully processed.
   * <p><b>NB:</b> content is volatile, so you must copy its contents if you use
   * it beyond the lifetime of this call.
   * @param content CharArrayWriter containing the content of the tag.
   */
  public void tagContent(CharArrayWriter content)
  {
    // Ignore
  }

  public void fixTransforms()
  {
    DNode     to;
    Iterator  it = txmap.iterator();

    while(it.hasNext()) {
      tx x = (tx) it.next();

      //Logger.log(Logger.DEBUG,"Fixing transform "+x.toID,x.transform,Integer.toString(x.node.getID()),Integer.toString(module.getNode(x.toID).getID()));
      to    = module.getNode(x.toID);

      x.transform.setFrom(x.node);
      x.transform.setTo(to);
      //to.setFrom(x.transform);
    }

  }

  /**
   * Parse an InputSource and return the contained module.
   * @return DModule loaded, null if the xml file does not contain a module.
   */
  public DModule parse(InputSource is)
  throws IOException,SAXException
  {
    getTagHandler().parse(is);
    fixTransforms();
    return module;
  }

  /**
   * Parse an uri and return the contained module.
   * @return DModule loaded, null if the xml file does not contain a module.
   */
  public DModule parse(String uri)
  throws IOException,SAXException
  {
    getTagHandler().parse(uri);
    fixTransforms();
    return module;
  }

  /**
   * Debug test - read xml from one file and save to another.
   */
  public static void main(String args[]) throws Exception
  {
    if(args.length!=2) {
      System.err.println("Syntax: java DModuleXML in-file out-file");
      System.exit(1);
    }

    Logger.setLevel(Logger.DEBUG);

    Logger.log(Logger.INFO,"DModuleXML Read test1.xml");
    DModuleXML dm = new DModuleXML();
    DModule module = dm.parse(new InputSource(new FileInputStream(args[0])));

    Logger.log(Logger.INFO,"Parse complete");

    Logger.log(Logger.INFO,"DModuleXML Write XML");
    FileWriter fw = new FileWriter(args[1]);
    module.saveXML(new XMLFactory(fw));
    fw.close();
    Logger.log(Logger.INFO,"Write complete");

    DProcessor.run(module);
  }
}