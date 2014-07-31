/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2010 Apple Inc. All rights reserved.
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
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <mach/mach_error.h>

// from _simple.h in libc
typedef struct _SIMPLE*	_SIMPLE_STRING;
extern void				_simple_vdprintf(int __fd, const char *__fmt, va_list __ap);
extern void				_simple_dprintf(int __fd, const char *__fmt, ...);
extern _SIMPLE_STRING	_simple_salloc(void);
extern int				_simple_vsprintf(_SIMPLE_STRING __b, const char *__fmt, va_list __ap);
extern void				_simple_sfree(_SIMPLE_STRING __b);
extern char *			_simple_string(_SIMPLE_STRING __b);

// dyld::log(const char* format, ...)
extern void _ZN4dyld3logEPKcz(const char*, ...);

// dyld::halt(const char* msg);
extern void _ZN4dyld4haltEPKc(const char* msg) __attribute__((noreturn));


// abort called by C++ unwinding code
void abort()
{	
	_ZN4dyld4haltEPKc("dyld calling abort()\n");
}

// std::terminate called by C++ unwinding code
void _ZSt9terminatev()
{
	_ZN4dyld4haltEPKc("dyld std::terminate()\n");
}

// std::unexpected called by C++ unwinding code
void _ZSt10unexpectedv()
{
	_ZN4dyld4haltEPKc("dyld std::unexpected()\n");
}

// __cxxabiv1::__terminate(void (*)()) called to terminate process
void _ZN10__cxxabiv111__terminateEPFvvE()
{
	_ZN4dyld4haltEPKc("dyld std::__terminate()\n");
}

// __cxxabiv1::__unexpected(void (*)()) called to terminate process
void _ZN10__cxxabiv112__unexpectedEPFvvE()
{
	_ZN4dyld4haltEPKc("dyld std::__unexpected()\n");
}

// __cxxabiv1::__terminate_handler
void* _ZN10__cxxabiv119__terminate_handlerE  = &_ZSt9terminatev;

// __cxxabiv1::__unexpected_handler
void* _ZN10__cxxabiv120__unexpected_handlerE = &_ZSt10unexpectedv;

// libc uses assert()
void __assert_rtn(const char* func, const char* file, int line, const char* failedexpr)
{
	if (func == NULL)
		_ZN4dyld3logEPKcz("Assertion failed: (%s), file %s, line %d.\n", failedexpr, file, line);
	else
		_ZN4dyld3logEPKcz("Assertion failed: (%s), function %s, file %s, line %d.\n", failedexpr, func, file, line);
	abort();
}


// called by libuwind code before aborting
size_t fwrite(const void* ptr, size_t size, size_t nitme, FILE* stream)
{
	return fprintf(stream, "%s", (char*)ptr); 
}

// called by libuwind code before aborting
int	fprintf(FILE* file, const char* format, ...)
{
	va_list	list;
	va_start(list, format);
	_simple_vdprintf(STDERR_FILENO, format, list);
	va_end(list);
	return 0;
}

// called by LIBC_ABORT
void abort_report_np(const char* format, ...)
{
	va_list list;
	const char *str;
	_SIMPLE_STRING s = _simple_salloc();
	if ( s != NULL ) {
		va_start(list, format);
		_simple_vsprintf(s, format, list);
		va_end(list);
		str = _simple_string(s);
	} 
	else {
		// _simple_salloc failed, but at least format may have useful info by itself
		str = format; 
	}
	_ZN4dyld4haltEPKc(str);
	// _ZN4dyld4haltEPKc doesn't return, so we can't call _simple_sfree
}


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

// malloc calls exit(-1) in case of errors...
void exit(int x)
{
	_ZN4dyld4haltEPKc("exit()");
}

// static initializers make calls to __cxa_atexit
void __cxa_atexit()
{
	// do nothing, dyld never terminates
}

//
// The stack protector routines in lib.c bring in too much stuff, so 
// make our own custom ones.
//
long __stack_chk_guard = 0;
static __attribute__((constructor)) 
void __guard_setup(int argc, const char* argv[], const char* envp[], const char* apple[])
{
	for (const char** p = apple; *p != NULL; ++p) {
		if ( strncmp(*p, "stack_guard=", 12) == 0 ) {
			// kernel has provide a random value for us
			for (const char* s = *p + 12; *s != '\0'; ++s) {
				char c = *s;
				long value = 0;
				if ( (c >= 'a') && (c <= 'f') )
					value = c - 'a' + 10;
				else if ( (c >= 'A') && (c <= 'F') )
					value = c - 'A' + 10;
				else if ( (c >= '0') && (c <= '9') )
					value = c - '0';
				__stack_chk_guard <<= 4;
				__stack_chk_guard |= value;
			}
			if ( __stack_chk_guard != 0 )
				return;
		}
	}
	
#if __LP64__
	__stack_chk_guard = ((long)arc4random() << 32) | arc4random();
#else
	__stack_chk_guard = arc4random();
#endif
}
extern void _ZN4dyld4haltEPKc(const char*);
void __stack_chk_fail()
{
	_ZN4dyld4haltEPKc("stack buffer overrun");
}


// std::_throw_bad_alloc()
void _ZSt17__throw_bad_allocv()
{
	_ZN4dyld4haltEPKc("__throw_bad_alloc()");
}

// std::_throw_length_error(const char* x)
void _ZSt20__throw_length_errorPKc()
{
	_ZN4dyld4haltEPKc("_throw_length_error()");
}

// the libc.a version of this drags in ASL
void __chk_fail()
{
	_ZN4dyld4haltEPKc("__chk_fail()");
}


// referenced by libc.a(pthread.o) but unneeded in dyld
void _init_cpu_capabilities() { }
void _cpu_capabilities() {}
void set_malloc_singlethreaded() {}
int PR_5243343_flag = 0;


// used by some pthread routines
char* mach_error_string(mach_error_t err)
{
	return (char *)"unknown error code";
}
char* mach_error_type(mach_error_t err)
{
	return (char *)"(unknown/unknown)";
}

// _pthread_reap_thread calls fprintf(stderr). 
// We map fprint to _simple_vdprintf and ignore FILE* stream, so ok for it to be NULL
FILE* __stderrp = NULL;
FILE* __stdoutp = NULL;

// work with c++abi.a
void (*__cxa_terminate_handler)() = _ZSt9terminatev;
void (*__cxa_unexpected_handler)() = _ZSt10unexpectedv;

void abort_message(const char* format, ...)
{
	va_list	list;
	va_start(list, format);
	_simple_vdprintf(STDERR_FILENO, format, list);
	va_end(list);
}	

void __cxa_bad_typeid()
{
	_ZN4dyld4haltEPKc("__cxa_bad_typeid()");
}

// to work with libc++
void _ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv()
{
	_ZN4dyld4haltEPKc("std::vector<>::_throw_length_error()");
}

// libc.a sometimes missing memset
#undef memset
void* memset(void* b, int c, size_t len)
{
	uint8_t* p = (uint8_t*)b;
	for(size_t i=len; i > 0; --i)
		*p++ = c;
	return b;
}

