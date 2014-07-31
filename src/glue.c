/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2007 Apple Inc. All rights reserved.
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
#include <time.h>
#include <unistd.h>

 
// abort called by C++ unwinding code
void abort()
{
	_exit(1);
}

// std::terminate called by C++ unwinding code
void _ZSt9terminatev()
{
	_exit(1);
}

// std::unexpected called by C++ unwinding code
void _ZSt10unexpectedv()
{
	_exit(1);
}

// __cxxabiv1::__terminate(void (*)()) called to terminate process
void _ZN10__cxxabiv111__terminateEPFvvE()
{
	_exit(1);
}

// __cxxabiv1::__unexpected(void (*)()) called to terminate process
void _ZN10__cxxabiv112__unexpectedEPFvvE()
{
	_exit(1);
}

// __cxxabiv1::__terminate_handler
void* _ZN10__cxxabiv119__terminate_handlerE  = &_ZSt9terminatev;

// __cxxabiv1::__unexpected_handler
void* _ZN10__cxxabiv120__unexpected_handlerE = &_ZSt10unexpectedv;




// real cthread_set_errno_self() has error handling that pulls in 
// pthread_exit() which pulls in fprintf()
extern int* __error(void);
void cthread_set_errno_self(int err)
{
	int* ep = __error();
     *ep = err;
}

/*
 * We have our own localtime() to avoid needing the notify API which is used
 * by the code in libc.a for localtime() which is used by arc4random().
 */
struct tm* localtime(const time_t* t)
{
	return (struct tm*)NULL;
}


