/*-------------------------------------------------------------------------
 *
 * color.c
 *	  PostgreSQL type definition for a custom 'color' data type.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "libpq/pqformat.h"

typedef uint32 color;

#define COLOR_RED_SHIFT 16
#define COLOR_GREEN_SHIFT 8
#define COLOR_BLUE_SHIFT 0
#define COLOR_CHANNEL_MASK 0xFF

/*
 * color_in - Color reader. Accepts a hexadecimal string as the input, and converts it to a color value.
 */
PG_FUNCTION_INFO_V1(color_in);
Datum color_in(PG_FUNCTION_ARGS)
{
    char *str = PG_GETARG_CSTRING(0);
    uint32 c = 0;

    if (sscanf(str, "%06X", &c) != 1) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                        errmsg("invalid input syntax for type color: \"%s\"", str)));
    }

    PG_RETURN_UINT32(c);
}

/*
 * color_out - Color output function. Converts the internal color value to a hexadecimal string representation.
 */
PG_FUNCTION_INFO_V1(color_out);
Datum color_out(PG_FUNCTION_ARGS)
{
    uint32 c = PG_GETARG_UINT32(0);
    char str[7];

    // Format color as a 6-character hexadecimal string
    snprintf(str, sizeof(str), "%06X", c);
    PG_RETURN_CSTRING(pstrdup(str));
}

/*
 * color_send - converts color to binary format
 */
PG_FUNCTION_INFO_V1(color_send);
Datum color_send(PG_FUNCTION_ARGS)
{
    uint32 c = PG_GETARG_UINT32(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendint(&buf, c, 4);
    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * color_recv - converts external binary format back to the internal color type
 */
PG_FUNCTION_INFO_V1(color_recv);
Datum color_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);
    uint32 c;

    c = pq_getmsgint(buf, 4);
    PG_RETURN_UINT32(c);
}

/*
 * color_eq - checks for equality between two values of type color
 */
PG_FUNCTION_INFO_V1(color_eq);
Datum color_eq(PG_FUNCTION_ARGS)
{
    color arg1 = PG_GETARG_UINT32(0);
    color arg2 = PG_GETARG_UINT32(1);

    PG_RETURN_BOOL(arg1 == arg2);
}

/*
 * color_ne - checks for inequality between two values of type color
 */
PG_FUNCTION_INFO_V1(color_ne);
Datum color_ne(PG_FUNCTION_ARGS)
{
    color arg1 = PG_GETARG_UINT32(0);
    color arg2 = PG_GETARG_UINT32(1);

    PG_RETURN_BOOL(arg1 != arg2);
}

/*
 * color_add - adds two values of type color and returns the result
 */
PG_FUNCTION_INFO_V1(color_add);
Datum color_add(PG_FUNCTION_ARGS)
{
    color arg1 = PG_GETARG_UINT32(0);
    color arg2 = PG_GETARG_UINT32(1);

    // Extract the Red (R) channel component of arg1 and arg2 by shifting the bits
    // to the right and applying a bit mask to isolate the 8 bits representing the Red channel.
    uint32 r1 = (arg1 >> COLOR_RED_SHIFT) & COLOR_CHANNEL_MASK;
    uint32 r2 = (arg2 >> COLOR_RED_SHIFT) & COLOR_CHANNEL_MASK;

    // Extract the Green (G) channel component.
    uint32 g1 = (arg1 >> COLOR_GREEN_SHIFT) & COLOR_CHANNEL_MASK;
    uint32 g2 = (arg2 >> COLOR_GREEN_SHIFT) & COLOR_CHANNEL_MASK;

    // Extract the Blue (B) channel component.
    uint32 b1 = (arg1 >> COLOR_BLUE_SHIFT) & COLOR_CHANNEL_MASK;
    uint32 b2 = (arg2 >> COLOR_BLUE_SHIFT) & COLOR_CHANNEL_MASK;

    // Add the Red (R) channel components, ensuring that the result does not exceed
    // the maximum value represented by COLOR_CHANNEL_MASK (0xFF), which corresponds
    // to the upper limit of the Red channel. This prevents overflow.
    uint32 r = Min(r1 + r2, COLOR_CHANNEL_MASK);

    // Add the Green (G) channel components.
    uint32 g = Min(g1 + g2, COLOR_CHANNEL_MASK);

    // Add the Blue (B) channel components.
    uint32 b = Min(b1 + b2, COLOR_CHANNEL_MASK);

    // Reconstruct the color by left-shifting the Red (R), Green (G), and Blue (B)
    // components back to their original bit positions and combining them with the OR (|) operation.
    color result_color = (r << COLOR_RED_SHIFT) | (g << COLOR_GREEN_SHIFT) | (b << COLOR_BLUE_SHIFT);

    // Return the final color as a uint32 value.
    PG_RETURN_UINT32(result_color);
}
