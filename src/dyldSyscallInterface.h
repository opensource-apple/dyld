/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2013 Apple Inc. All rights reserved.
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


#ifndef __DYLD_SYSCALL_HELPERS__
#define __DYLD_SYSCALL_HELPERS__


#if __cplusplus
namespace dyld {
#endif

	//
	// This file contains the table of function pointers the host dyld supplies
	// to the iOS simulator dyld.
	//
	struct SyscallHelpers
	{
		uintptr_t		version;
		int				(*open)(const char* path, int oflag, int extra);
		int				(*close)(int fd);
		ssize_t			(*pread)(int fd, void* buf, size_t nbyte, off_t offset);
		ssize_t			(*write)(int fd, const void* buf, size_t nbyte);
		void*			(*mmap)(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
		int				(*munmap)(void* addr, size_t len);
		int				(*madvise)(void* addr, size_t len, int advice);
		int				(*stat)(const char* path, struct stat* buf);
		int				(*fcntl)(int fildes, int cmd, void* result);
		int				(*ioctl)(int fildes, unsigned long request, void* result);
		int				(*issetugid)(void);
		char*			(*getcwd)(char* buf, size_t size);
		char*			(*realpath)(const char* file_name, char* resolved_name);
		kern_return_t	(*vm_allocate)(vm_map_t target_task, vm_address_t *address, vm_size_t size, int flags);
		kern_return_t	(*vm_deallocate)(vm_map_t target_task, vm_address_t address, vm_size_t size);
		kern_return_t	(*vm_protect)(vm_map_t target_task, vm_address_t address, vm_size_t size, boolean_t max, vm_prot_t prot);
		void			(*vlog)(const char* format, va_list list);
		void			(*vwarn)(const char* format, va_list list);
		int				(*pthread_mutex_lock)(pthread_mutex_t* m);
		int				(*pthread_mutex_unlock)(pthread_mutex_t* m);
		mach_port_t		(*mach_thread_self)(void);
		kern_return_t	(*mach_port_deallocate)(ipc_space_t task, mach_port_name_t name);
		mach_port_name_t(*task_self_trap)(void);
		kern_return_t   (*mach_timebase_info)(mach_timebase_info_t info);
		bool			(*OSAtomicCompareAndSwapPtrBarrier)(void* old, void* nw, void * volatile *value);
		void			(*OSMemoryBarrier)(void);
		void*			(*getProcessInfo)(void); // returns dyld_all_image_infos*;
		int*			(*errnoAddress)();
		uint64_t		(*mach_absolute_time)();
	};

	extern const struct SyscallHelpers* gSyscallHelpers;


#if __cplusplus
}
#endif

#endif