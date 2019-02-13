/*---------------------------------------------------------------------------
 *
 * Ryu floating-point output.
 *
 * Portions Copyright (c) 2018-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/common/shortest_dec.h
 *
 * This is a modification of code taken from github.com/ulfjack/ryu under the
 * terms of the Boost license (not the Apache license). The original copyright
 * notice follows:
 *
 * Copyright 2018 Ulf Adams
 *
 * The contents of this file may be used under the terms of the Apache
 * License, Version 2.0.
 *
 *     (See accompanying file LICENSE-Apache or copy at
 *      http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * Boost Software License, Version 1.0.
 *
 *     (See accompanying file LICENSE-Boost or copy at
 *      https://www.boost.org/LICENSE_1_0.txt)
 *
 * Unless required by applicable law or agreed to in writing, this software is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.
 *
 *---------------------------------------------------------------------------
 */
#ifndef SHORTEST_DEC_H
#define SHORTEST_DEC_H

/*----
 * The length of 25 comes from:
 *
 * Case 1: -9.9999999999999999e-299  = 24 bytes, plus 1 for null
 *
 * Case 2: -0.00099999999999999999   = 23 bytes, plus 1 for null
 */
#define DOUBLE_SHORTEST_DECIMAL_LEN 25

int			double_to_shortest_decimal_bufn(double f, char *result);
int			double_to_shortest_decimal_buf(double f, char *result);
char	   *double_to_shortest_decimal(double f);

/*
 * The length of 16 comes from:
 *
 * Case 1: -9.99999999e+29  = 15 bytes, plus 1 for null
 *
 * Case 2: -0.000999999999  = 15 bytes, plus 1 for null
 */
#define FLOAT_SHORTEST_DECIMAL_LEN 16

int			float_to_shortest_decimal_bufn(float f, char *result);
int			float_to_shortest_decimal_buf(float f, char *result);
char	   *float_to_shortest_decimal(float f);

#endif							/* SHORTEST_DEC_H */
