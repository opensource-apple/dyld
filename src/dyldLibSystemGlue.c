/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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

#include <stdlib.h>

//
// <rdar://problem/6536810> Alter libdyld.a to not need libsystem to link with dylib1.o
// This is the temporary private interface between libSystem.B.dylib and dyld
//

#if __i386__ || __x86_64__
// The compiler driver will continue to add -ldylib1.o to ppc and arm links
// so only i386 and x86_64 need this extra glue.

struct __DATA__dyld { long lazy; int (*lookup)(const char*, void**); };

static struct __DATA__dyld  myDyldSection __attribute__ ((section ("__DATA,__dyld"))) = { 0, NULL };


__attribute__((weak, visibility("hidden"))) int _dyld_func_lookup(const char* dyld_func_name, void **address)
{
	return (*myDyldSection.lookup)(dyld_func_name, address);
}

#endif



