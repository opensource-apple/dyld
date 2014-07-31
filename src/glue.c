/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <notify.h>
#include <time.h>

//
// Stub functions needed for dyld to link with libc.a instead of libSystem.dylib
//
//
static char* dyld_progname = "dyld";

//
// libc has calls to _dyld_lookup_and_bind to find args and environment.  Since
// dyld links with a staic copy of libc, those calls need to find dyld's args and env
// not the host process.  We implement a special _dyld_lookup_and_bind() that
// just knows how to bind the few symbols needed by dyld.
//
void _dyld_lookup_and_bind(const char* symbolName, void** address, void* module)
{
	if ( strcmp(symbolName, "_environ") == 0 ) {
		// dummy up empty environment list
		static char *p = NULL;
		static char **pp = &p;
		*address = &pp;
		return;
	}
	else if ( strcmp(symbolName, "___progname") == 0 ) {
		*address = &dyld_progname;
		return;
	}
	
	fprintf(stderr, "dyld: internal error: unknown symbol '%s'\n", symbolName);
}


int NSIsSymbolNameDefined(const char* symbolName)
{
	if ( strcmp(symbolName, "___progname") == 0 ) {
		return 1;
	}
	fprintf(stderr, "dyld: internal error: unknown symbol '%s'\n", symbolName);
	return 0;
}


/*
 * To avoid linking in libm.  These variables are defined as they are used in
 * pthread_init() to put in place a fast sqrt().
 */
size_t hw_sqrt_len = 0;

double
sqrt(double x)
{
	return(0.0);
}
double
hw_sqrt(double x)
{
	return(0.0);
}

/*
 * More stubs to avoid linking in libm.  This works as along as we don't use
 * long doubles.
 */
long
__fpclassifyd(double x) 
{
	return(0);
}

long
__fpclassify(long double x)
{
	return(0);
}


char* __hdtoa(double d, const char *xdigs, int ndigits, int *decpt, int *sign, char **rve)
{
	return NULL;
}

char* __hldtoa(/*long*/ double e, const char *xdigs, int ndigits, int *decpt, int *sign, char **rve)
{
	return NULL;
}

int __fegetfltrounds(void) 
{
       return 1;                                       /* FE_NEAREST */
}

/*
 * We have our own localtime() to avoid needing the notify API which is used
 * by the code in libc.a for localtime() but is in libnotify.
 */
struct tm* localtime(const time_t* t)
{
	return (struct tm*)NULL;
}

