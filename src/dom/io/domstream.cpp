/*
 * Phoebe DOM Implementation.
 *
 * This is a C++ approximation of the W3C DOM model, which follows
 * fairly closely the specifications in the various .idl files, copies of
 * which are provided for reference.  Most important is this one:
 *
 * http://www.w3.org/TR/2004/REC-DOM-Level-3-Core-20040407/idl-definitions.html
 *
 * Authors:
 *   Bob Jamison
 *
 * Copyright (C) 2006-2008 Bob Jamison
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * Our base input/output stream classes.  These are is directly
 * inherited from iostreams, and includes any extra
 * functionality that we might need.
 *
 */

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "domstream.h"
#include "dom/ucd.h"

namespace org
{
namespace w3c
{
namespace dom
{
namespace io
{


//#########################################################################
//# U T I L I T Y
//#########################################################################

void pipeStream(InputStream &source, OutputStream &dest)
{
    for (;;)
        {
        int ch = source.get();
        if (ch<0)
            break;
        dest.put(ch);
        }
    dest.flush();
}
/*


//#########################################################################
//# F O R M A T T E D    P R I N T I N G
//#########################################################################

static char const *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
*/
// static int dprintInt(Writer &outs,
                     // long arg, int base,
                     // int flag, int width, int /*precision*/)
/*{

    DOMString buf;

    //### Get the digits
    while (arg > 0)
        {
        int ch = arg % base;
        buf.insert(buf.begin(), digits[ch]);        
        arg /= base;        
        }

    if (flag == '#' && base == 16)
        {
        buf.insert(buf.begin(), 'x');
        buf.insert(buf.begin(), '0');
        }

    if (buf.size() == 0)
        buf = "0";

    int pad = width - (int)buf.size();
    for (int i=0 ; i<pad ; i++)
        buf.insert(buf.begin(), '0');

    //### Output the result
    for (unsigned int i=0 ; i<buf.size() ; i++)
        {
        if (outs.put(buf[i]) < 0)
            return -1;
        }

    return 1;
}



static int dprintDouble(Writer &outs, double val, 
                        int flag, int width, int precision)
{

    DOMString buf;

    //printf("int:%f  frac:%f\n", intPart, fracPart);

    bool negative = false;
    if (val < 0)
        {
        negative = true;
        val = -val;
        }

    int intDigits = 0;
    double scale = 1.0;
    while (scale < val)
        {
        intDigits++;
        scale *= 10.0;
        }

    double intPart;
    double fracPart = modf(val, &intPart);

    if (precision <= 0)
        precision = 5;

    //### How many pad digits?
    int pad = width - intDigits;
    if (precision > 0)
        pad -= precision + 1;
    else if (flag == '#')
        pad--;


    //### Signs
    if (negative)
        buf.push_back('-');
    else if (flag == '+')
        buf.push_back('+');

    //### Prefix pad
    if (pad > 0 && flag == '0')
        {
        while (pad--)
            buf.push_back('0');
        }

    //### Integer digits
    intPart = (intPart + 0.1 ) / scale;    // turn 12345.678 to .12345678
    while (intDigits--)
        {
        intPart *= 10.0;
        double dig;
        intPart = modf(intPart, &dig);
        char ch = '0' + (int)dig;
        buf.push_back(ch);
        }
    if (buf.size() == 0)
        buf = "0";

    //### Decimal point
    if (flag == '#' || precision > 0)
        {
        buf.push_back('.');
        }    

    //### Fractional digits
    while (precision--)
        {
        fracPart *= 10.0;
        double dig;
        fracPart = modf(fracPart, &dig);
        char ch = '0' + (int)dig;
        buf.push_back(ch);
        }

    //### Left justify if requested
    if (pad > 0 && flag == '-')
        {
        while (pad--)
            buf.push_back(' ');
        }

    //### Output the result
    for (unsigned int i=0 ; i<buf.size() ; i++)
        {
        if (outs.put(buf[i]) < 0)
            return -1;
        }
    return 1;
}
*/

/**
 * Output a string.  We veer from the standard a tiny bit.
 * Normally, a flag of '#' is undefined for strings.  We use
 * it as an indicator that the user wants to XML-escape any
 * XML entities.
 */
// static int dprintString(Writer &outs, const DOMString &str,
                        // int flags, int /*width*/, int /*precision*/)
/*{
    int len = str.size();
    if (flags == '#')
        {
        for (int pos = 0; pos < len; pos++)
            {
            XMLCh ch = (XMLCh) str[pos];
            if (ch == '&')
                outs.writeString("&ampr;");
            else if (ch == '<')
                outs.writeString("&lt;");
            else if (ch == '>')
                outs.writeString("&gt;");
            else if (ch == '"')
                outs.writeString("&quot;");
            else if (ch == '\'')
                outs.writeString("&apos;");
            else
                outs.put(ch);
            }
        }
    else
        {
        outs.writeString(str);
        }

    return 1;
}



static int getint(const DOMString &buf, int pos, int *ret)
{
    int len = buf.size();
    if (!len)
        {
        *ret = 0;
        return pos;
        }

    bool has_sign = false;
    int val = 0;
    if (buf[pos] == '-')
        {
        has_sign = true;
        pos++;
        }
    while (pos < len)
        {
        XMLCh ch = buf[pos];
        if (ch >= '0' && ch <= '9')
            val = val * 10 + (ch - '0');
        else
            break;
        pos++;
        }
    if (has_sign)
        val = -val;

    *ret = val;

    return pos;
}



static int dprintf(Writer &outs, const DOMString &fmt, va_list ap)
{

    int len = fmt.size();

    for (int pos=0 ; pos < len ; pos++)
        {
        XMLCh ch = fmt[pos];

        //## normal character
        if (ch != '%')
            {
            if (outs.put(ch)<0)
                {
                return -1;
                }
            continue;
            }

        if (++pos >= len)
            {
            return -1;
            }

        ch = fmt[pos];

        //## is this %% ?
        if (ch == '%') // escaped '%'
            {
            if (outs.put('%')<0)
                {
                return -1;
                }
            continue;
            }

        //## flag
        char flag = '\0';
        if (ch == '-' || ch == '+' || ch == ' ' ||
            ch == '#' || ch == '0')
            {
            flag = ch;
            if (++pos >= len)
                {
                return -1;
                }
            ch = fmt[pos];
            }

        //## width.precision
        int width     = 0;
        int precision = 0;
        pos = getint(fmt, pos, &width);
        if (pos >= len)
            {
            return -1;
            }
        ch = fmt[pos];
        if (ch == '.')
            {
            if (++pos >= len)
                {
                return -1;
                }
            pos = getint(fmt, pos, &precision);
            if (pos >= len)
                {
                return -1;
                }
            ch = fmt[pos];
            }

        //## length
        char length = '\0';
        if (ch == 'l' || ch == 'h')
            {
            length = ch;
            if (++pos >= len)
                {
                return -1;
                }
            ch = fmt[pos];
            }

        //## data type
        switch (ch)
            {
            case 'f':
            case 'g':
                {
                double val = va_arg(ap, double);
                dprintDouble(outs, val, flag, width, precision);
                break;
                }
            case 'd':
                {
                long val = 0;
                if (length == 'l')
                    val = va_arg(ap, long);
                else if (length == 'h')
                    val = (long)va_arg(ap, int);
                else
                    val = (long)va_arg(ap, int);
                dprintInt(outs, val, 10, flag, width, precision);
                break;
                }
            case 'x':
                {
                long val = 0;
                if (length == 'l')
                    val = va_arg(ap, long);
                else if (length == 'h')
                    val = (long)va_arg(ap, int);
                else
                    val = (long)va_arg(ap, int);
                dprintInt(outs, val, 16, flag, width, precision);
                break;
                }
            case 's':
                {
                DOMString val = va_arg(ap, char *);
                dprintString(outs, val, flag, width, precision);
                break;
                }
            default:
                {
                break;
                }
            }
        }

    return 1;
}
*/

//#########################################################################
//# B A S I C    I N P U T    S T R E A M
//#########################################################################

/**
 *
 */ 
BasicInputStream::BasicInputStream(InputStream &sourceStream)
                   : source(sourceStream)
{
    closed = false;
}

/**
 * Returns the number of bytes that can be read (or skipped over) from
 * this input stream without blocking by the next caller of a method for
 * this input stream.
 */ 
int BasicInputStream::available()
{
    if (closed)
        return 0;
    return source.available();
}

    
/**
 *  Closes this input stream and releases any system resources
 *  associated with the stream.
 */ 
void BasicInputStream::close()
{
    if (closed)
        return;
    source.close();
    closed = true;
}
    
/**
 * Reads the next byte of data from the input stream.  -1 if EOF
 */ 
int BasicInputStream::get()
{
    if (closed)
        return -1;
    return source.get();
}



//#########################################################################
//# B A S I C    O U T P U T    S T R E A M
//#########################################################################

/**
 *
 */ 
BasicOutputStream::BasicOutputStream(OutputStream &destinationStream)
                     : destination(destinationStream)
{
    closed = false;
}

/**
 * Closes this output stream and releases any system resources
 * associated with this stream.
 */ 
void BasicOutputStream::close()
{
    if (closed)
        return;
    destination.close();
    closed = true;
}
    
/**
 *  Flushes this output stream and forces any buffered output
 *  bytes to be written out.
 */ 
void BasicOutputStream::flush()
{
    if (closed)
        return;
    destination.flush();
}
    
/**
 * Writes the specified byte to this output stream.
 */ 
int BasicOutputStream::put(gunichar ch)
{
    if (closed)
        return -1;
    destination.put(ch);
    return 1;
}


//#########################################################################
//# B A S I C    R E A D E R
//#########################################################################
BasicReader::BasicReader(Reader &sourceReader)
{
    source = &sourceReader;
}

/**
 * Returns the number of bytes that can be read (or skipped over) from
 * this reader without blocking by the next caller of a method for
 * this reader.
 */ 
int BasicReader::available()
{
    if (source)
        return source->available();
    else
        return 0;
}

    
/**
 *  Closes this reader and releases any system resources
 *  associated with the reader.
 */ 
void BasicReader::close()
{
    if (source)
        source->close();
}
    
/**
 * Reads the next byte of data from the reader.
 */ 
gunichar BasicReader::get()
{
    if (source)
        return source->get();
    else
        return (gunichar)-1;
}
   

/**
 * Reads a line of data from the reader.
 */ 
Glib::ustring BasicReader::readLine()
{
    Glib::ustring str;
    while (available() > 0)
        {
        gunichar ch = get();
        if (ch == '\n')
            break;
        str.push_back(ch);
        }
    return str;
}
   
/**
 * Reads a line of data from the reader.
 */ 
Glib::ustring BasicReader::readWord()
{
    Glib::ustring str;
    while (available() > 0)
        {
        gunichar ch = get();
        if (!g_unichar_isprint(ch))
            break;
        str.push_back(ch);
        }
    return str;
}
   

static bool getLong(Glib::ustring &str, long *val)
{
    const char *begin = str.raw().c_str();
    char *end;
    long ival = strtol(begin, &end, 10);
    if (str == end)
        return false;
    *val = ival;
    return true;
}

static bool getULong(Glib::ustring &str, unsigned long *val)
{
    const char *begin = str.raw().c_str();
    char *end;
    unsigned long ival = strtoul(begin, &end, 10);
    if (str == end)
        return false;
    *val = ival;
    return true;
}

static bool getDouble(Glib::ustring &str, double *val)
{
    const char *begin = str.raw().c_str();
    char *end;
    double ival = strtod(begin, &end);
    if (str == end)
        return false;
    *val = ival;
    return true;
}



const Reader &BasicReader::readBool (bool& val )
{
    Glib::ustring buf = readWord();
    if (buf == "true")
        val = true;
    else
        val = false;
    return *this;
}

const Reader &BasicReader::readShort (short& val )
{
    Glib::ustring buf = readWord();
    long ival;
    if (getLong(buf, &ival))
        val = (short) ival;
    return *this;
}

const Reader &BasicReader::readUnsignedShort (unsigned short& val )
{
    Glib::ustring buf = readWord();
    unsigned long ival;
    if (getULong(buf, &ival))
        val = (unsigned short) ival;
    return *this;
}

const Reader &BasicReader::readInt (int& val )
{
    Glib::ustring buf = readWord();
    long ival;
    if (getLong(buf, &ival))
        val = (int) ival;
    return *this;
}

const Reader &BasicReader::readUnsignedInt (unsigned int& val )
{
    Glib::ustring buf = readWord();
    unsigned long ival;
    if (getULong(buf, &ival))
        val = (unsigned int) ival;
    return *this;
}

const Reader &BasicReader::readLong (long& val )
{
    Glib::ustring buf = readWord();
    long ival;
    if (getLong(buf, &ival))
        val = ival;
    return *this;
}

const Reader &BasicReader::readUnsignedLong (unsigned long& val )
{
    Glib::ustring buf = readWord();
    unsigned long ival;
    if (getULong(buf, &ival))
        val = ival;
    return *this;
}

const Reader &BasicReader::readFloat (float& val )
{
    Glib::ustring buf = readWord();
    double ival;
    if (getDouble(buf, &ival))
        val = (float)ival;
    return *this;
}

const Reader &BasicReader::readDouble (double& val )
{
    Glib::ustring buf = readWord();
    double ival;
    if (getDouble(buf, &ival))
        val = ival;
    return *this;
}



//#########################################################################
//# I N P U T    S T R E A M    R E A D E R
//#########################################################################


InputStreamReader::InputStreamReader(InputStream &inputStreamSource)
                     : inputStream(inputStreamSource)
{
}

    

/**
 *  Close the underlying OutputStream
 */
void InputStreamReader::close()
{
    inputStream.close();
}
    
/**
 *  Flush the underlying OutputStream
 */
int InputStreamReader::available()
{
    return inputStream.available();
}
    
/**
 *  Overloaded to receive its bytes from an InputStream
 *  rather than a Reader
 */
gunichar InputStreamReader::get()
{
    //Do we need conversions here?
    gunichar ch = (gunichar)inputStream.get();
    return ch;
}



//#########################################################################
//# S T D    R E A D E R
//#########################################################################


/**
 *
 */
StdReader::StdReader()
{
    inputStream = new StdInputStream();
}

/**
 *
 */
StdReader::~StdReader()
{
    delete inputStream;
}

    

/**
 *  Close the underlying OutputStream
 */
void StdReader::close()
{
    inputStream->close();
}
    
/**
 *  Flush the underlying OutputStream
 */
int StdReader::available()
{
    return inputStream->available();
}
    
/**
 *  Overloaded to receive its bytes from an InputStream
 *  rather than a Reader
 */
gunichar StdReader::get()
{
    //Do we need conversions here?
    gunichar ch = (gunichar)inputStream->get();
    return ch;
}




//#########################################################################
//# B A S I C    W R I T E R
//#########################################################################
/**
 *
 */ 
BasicWriter::BasicWriter(const Writer &destinationWriter)
{
    destination = (Writer*) &destinationWriter;
}

/**
 * Closes this writer and releases any system resources
 * associated with this writer.
 */ 
void BasicWriter::close()
{
    if (destination)
        destination->close();
}
    
/**
 *  Flushes this output stream and forces any buffered output
 *  bytes to be written out.
 */ 
void BasicWriter::flush()
{
    if (destination)
        destination->flush();
}
    
/**
 * Writes the specified byte to this output writer.
 */ 
int BasicWriter::put(gunichar ch)
{
    if (destination && destination->put(ch)>=0)
        return 1;
    return -1;
}

/**
 * Provide printf()-like formatting
 */ 
Writer &BasicWriter::printf(char const *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    gchar *buf = g_strdup_vprintf(fmt, args);
    va_end(args);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}
/**
 * Writes the specified character to this output writer.
 */ 
Writer &BasicWriter::writeChar(char ch)
{
    gunichar uch = ch;
    put(uch);
    return *this;
}


/**
 * Writes the specified unicode string to this output writer.
 */ 
Writer &BasicWriter::writeUString(Glib::ustring &str)
{
    for (int i=0; i< (int)str.size(); i++)
        put(str[i]);
    return *this;
}

/**
 * Writes the specified standard string to this output writer.
 */ 
Writer &BasicWriter::writeStdString(std::string &str)
{
    Glib::ustring tmp(str);
    writeUString(tmp);
    return *this;
}

/**
 * Writes the specified character string to this output writer.
 */ 
Writer &BasicWriter::writeString(const char *str)
{
    Glib::ustring tmp;
    if (str)
        tmp = str;
    else
        tmp = "null";
    writeUString(tmp);
    return *this;
}




/**
 *
 */
Writer &BasicWriter::writeBool (bool val )
{
    if (val)
        writeString("true");
    else
        writeString("false");
    return *this;
}


/**
 *
 */
Writer &BasicWriter::writeShort (short val )
{
    gchar *buf = g_strdup_printf("%d", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
  return *this;
}



/**
 *
 */
Writer &BasicWriter::writeUnsignedShort (unsigned short val )
{
    gchar *buf = g_strdup_printf("%u", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeInt (int val)
{
    gchar *buf = g_strdup_printf("%d", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeUnsignedInt (unsigned int val)
{
    gchar *buf = g_strdup_printf("%u", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeLong (long val)
{
    gchar *buf = g_strdup_printf("%ld", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeUnsignedLong(unsigned long val)
{
    gchar *buf = g_strdup_printf("%lu", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeFloat(float val)
{
#if 1
    gchar *buf = g_strdup_printf("%8.3f", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
#else
    std::string tmp = ftos(val, 'g', 8, 3, 0);
    writeStdString(tmp);
#endif
    return *this;
}

/**
 *
 */
Writer &BasicWriter::writeDouble(double val)
{
#if 1
    gchar *buf = g_strdup_printf("%8.3f", val);
    if (buf) {
        writeString(buf);
        g_free(buf);
    }
#else
    std::string tmp = ftos(val, 'g', 8, 3, 0);
    writeStdString(tmp);
#endif
    return *this;
}




//#########################################################################
//# O U T P U T    S T R E A M    W R I T E R
//#########################################################################


OutputStreamWriter::OutputStreamWriter(OutputStream &outputStreamDest)
                     : outputStream(outputStreamDest)
{
}

    

/**
 *  Close the underlying OutputStream
 */
void OutputStreamWriter::close()
{
    flush();
    outputStream.close();
}
    
/**
 *  Flush the underlying OutputStream
 */
void OutputStreamWriter::flush()
{
      outputStream.flush();
}
    
/**
 *  Overloaded to redirect the output chars from the next Writer
 *  in the chain to an OutputStream instead.
 */
int OutputStreamWriter::put(gunichar ch)
{
    if (outputStream.put(ch)>=0)
        return 1;
    return -1;
}

//#########################################################################
//# S T D    W R I T E R
//#########################################################################


/**
 *  
 */
StdWriter::StdWriter()
{
    outputStream = new StdOutputStream();
}
    
/**
 *  
 */
StdWriter::~StdWriter()
{
    delete outputStream;
}

/**
 *  Close the underlying OutputStream
 */
void StdWriter::close()
{
    flush();
    outputStream->close();
}
    
/**
 *  Flush the underlying OutputStream
 */
void StdWriter::flush()
{
      outputStream->flush();
}
    
/**
 *  Overloaded to redirect the output chars from the next Writer
 *  in the chain to an OutputStream instead.
 */
int StdWriter::put(gunichar ch)
{
    if (outputStream && (outputStream->put(ch)>=0))
        return 1;
    return -1;
}



//###############################################
//# O P E R A T O R S
//###############################################
//# Normally these would be in the .h, but we
//# just want to be absolutely certain that these
//# are never multiply defined.  Easy to maintain,
//# though.  Just occasionally copy/paste these
//# into the .h , and replace the {} with a ;
//###############################################




const Reader& operator>> (Reader &reader, bool& val )
        { return reader.readBool(val); }

const Reader& operator>> (Reader &reader, short &val)
        { return reader.readShort(val); }

const Reader& operator>> (Reader &reader, unsigned short &val)
        { return reader.readUnsignedShort(val); }

const Reader& operator>> (Reader &reader, int &val)
        { return reader.readInt(val); }

const Reader& operator>> (Reader &reader, unsigned int &val)
        { return reader.readUnsignedInt(val); }

const Reader& operator>> (Reader &reader, long &val)
        { return reader.readLong(val); }

const Reader& operator>> (Reader &reader, unsigned long &val)
        { return reader.readUnsignedLong(val); }

const Reader& operator>> (Reader &reader, float &val)
        { return reader.readFloat(val); }

const Reader& operator>> (Reader &reader, double &val)
        { return reader.readDouble(val); }



Writer& operator<< (Writer &writer, char val)
    { return writer.writeChar(val); }

Writer& operator<< (Writer &writer, Glib::ustring &val)
    { return writer.writeUString(val); }

Writer& operator<< (Writer &writer, std::string &val)
    { return writer.writeStdString(val); }

Writer& operator<< (Writer &writer, char const *val)
    { return writer.writeString(val); }

Writer& operator<< (Writer &writer, bool val)
    { return writer.writeBool(val); }

Writer& operator<< (Writer &writer, short val)
    { return writer.writeShort(val); }

Writer& operator<< (Writer &writer, unsigned short val)
    { return writer.writeUnsignedShort(val); }

Writer& operator<< (Writer &writer, int val)
    { return writer.writeInt(val); }

Writer& operator<< (Writer &writer, unsigned int val)
    { return writer.writeUnsignedInt(val); }

Writer& operator<< (Writer &writer, long val)
    { return writer.writeLong(val); }

Writer& operator<< (Writer &writer, unsigned long val)
    { return writer.writeUnsignedLong(val); }

Writer& operator<< (Writer &writer, float val)
    { return writer.writeFloat(val); }

Writer& operator<< (Writer &writer, double val)
    { return writer.writeDouble(val); }

}  //namespace io
}  //namespace dom
}  //namespace w3c
}  //namespace org


//#########################################################################
//# E N D    O F    F I L E
//#########################################################################
