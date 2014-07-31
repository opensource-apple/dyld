/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2008 Apple Inc. All rights reserved.
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

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <mach-o/reloc.h>
#if __ppc__ || __ppc64__
	#include <mach-o/ppc/reloc.h>
#endif
#if __x86_64__
	#include <mach-o/x86_64/reloc.h>
#endif
#include "dyld.h"

#ifndef MH_PIE
	#define MH_PIE 0x200000 
#endif


#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define macho_segment_command	segment_command_64
	#define macho_section			section_64
	#define RELOC_SIZE				3
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define macho_segment_command	segment_command
	#define macho_section			section
	#define RELOC_SIZE				2
#endif

#if __x86_64__
	#define POINTER_RELOC X86_64_RELOC_UNSIGNED
#else
	#define POINTER_RELOC GENERIC_RELOC_VANILLA
#endif

// from dyld.cpp
namespace dyld { extern bool isRosetta(); };


//
//  Code to bootstrap dyld into a runnable state
//
//

namespace dyldbootstrap {


typedef void (*Initializer)(int argc, const char* argv[], const char* envp[], const char* apple[]);

//
// For a regular executable, the crt code calls dyld to run the executables initializers.
// For a static executable, crt directly runs the initializers.
// dyld (should be static) but is a dynamic executable and needs this hack to run its own initializers.
// We pass argc, argv, etc in case libc.a uses those arguments
//
static void runDyldInitializers(const struct macho_header* mh, intptr_t slide, int argc, const char* argv[], const char* envp[], const char* apple[])
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_MOD_INIT_FUNC_POINTERS ){
							Initializer* inits = (Initializer*)(sect->addr + slide);
							const uint32_t count = sect->size / sizeof(uintptr_t);
							for (uint32_t i=0; i < count; ++i) {
								Initializer func = inits[i];
								func(argc, argv, envp, apple);
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

//
// If the kernel does not load dyld at its preferred address, we need to apply 
// fixups to various initialized parts of the __DATA segment
//
static void rebaseDyld(const struct macho_header* mh, intptr_t slide)
{
	// rebase non-lazy pointers (which all point internal to dyld, since dyld uses no shared libraries)
	// and get interesting pointers into dyld
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	const struct macho_segment_command* linkEditSeg = NULL;
#if __x86_64__
	const struct macho_segment_command* firstWritableSeg = NULL;
#endif
	const struct dysymtab_command* dynamicSymbolTable = NULL;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					if ( strcmp(seg->segname, "__LINKEDIT") == 0 )
						linkEditSeg = seg;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
							// rebase non-lazy pointers (which all point internal to dyld, since dyld uses no shared libraries)
							const uint32_t pointerCount = sect->size / sizeof(uintptr_t);
							uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + slide);
							for (uint32_t j=0; j < pointerCount; ++j) {
								symbolPointers[j] += slide;
							}
						}
					}
#if __x86_64__
					if ( (firstWritableSeg == NULL) && (seg->initprot & VM_PROT_WRITE) )
						firstWritableSeg = seg;
#endif
				}
				break;
			case LC_DYSYMTAB:
				dynamicSymbolTable = (struct dysymtab_command *)cmd;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	// use reloc's to rebase all random data pointers
#if __x86_64__
	const uintptr_t relocBase = firstWritableSeg->vmaddr + slide;
#else
	const uintptr_t relocBase = (uintptr_t)mh;
#endif
	const relocation_info* const relocsStart = (struct relocation_info*)(linkEditSeg->vmaddr + slide + dynamicSymbolTable->locreloff - linkEditSeg->fileoff);
	const relocation_info* const relocsEnd = &relocsStart[dynamicSymbolTable->nlocrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
	#if __ppc__ || __ppc64__ || __i36__
		if ( (reloc->r_address & R_SCATTERED) != 0 )
			throw "scattered relocation in dyld";
	#endif
		if ( reloc->r_length != RELOC_SIZE ) 
			throw "relocation in dyld has wrong size";

		if ( reloc->r_type != POINTER_RELOC ) 
			throw "relocation in dyld has wrong type";
		
		// update pointer by amount dyld slid
		*((uintptr_t*)(reloc->r_address + relocBase)) += slide;
	}
}


//
// For some reason the kernel loads dyld with __TEXT and __LINKEDIT writable
// rdar://problem/3702311 
//
static void segmentProtectDyld(const struct macho_header* mh, intptr_t slide)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					vm_address_t addr = seg->vmaddr + slide;
					vm_size_t size = seg->vmsize;
					const bool setCurrentPermissions = false;
					vm_protect(mach_task_self(), addr, size, setCurrentPermissions, seg->initprot);
					//dyld::log("dyld: segment %s, 0x%08X -> 0x%08X, set to %d\n", seg->segname, addr, addr+size-1, seg->initprot);
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
}


//
// re-map the main executable to a new random address
//
static const struct macho_header* randomizeExecutableLoadAddress(const struct macho_header* orgMH, const char* envp[], uintptr_t* appsSlide)
{
#if __ppc__
	// don't slide PIE programs running under rosetta
	if ( dyld::isRosetta() )
		return orgMH;
#endif
	// environment variable DYLD_NO_PIE can disable PIE
	for(const char** p = envp; *p != NULL; p++) {
		if ( strncmp(*p, "DYLD_NO_PIE=", 12) == 0 )
			return orgMH;
	}
	
	// count segments
	uint32_t segCount = 0;
	const uint32_t cmd_count = orgMH->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)orgMH)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			// page-zero and custom stacks don't move
			if ( (strcmp(segCmd->segname, "__PAGEZERO") != 0) && (strcmp(segCmd->segname, "__UNIXSTACK") != 0) ) 
				++segCount;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	// make copy of segment info
	macho_segment_command segs[segCount];
	uint32_t index = 0;
	uintptr_t highestAddressUsed = 0;
	uintptr_t lowestAddressUsed = UINTPTR_MAX;
	cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			if ( (strcmp(segCmd->segname, "__PAGEZERO") != 0) && (strcmp(segCmd->segname, "__UNIXSTACK") != 0) ) {
				segs[index++] = *segCmd;
				if ( (segCmd->vmaddr + segCmd->vmsize) > highestAddressUsed )
					highestAddressUsed = ((segCmd->vmaddr + segCmd->vmsize) + 4095) & -4096;
				if ( segCmd->vmaddr < lowestAddressUsed )
					lowestAddressUsed = segCmd->vmaddr;
				// do nothing if kernel has already randomized load address
				if ( (strcmp(segCmd->segname, "__TEXT") == 0) && (segCmd->vmaddr != (uintptr_t)orgMH) )
					return orgMH;
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	// choose a random new base address
#if __LP64__
	uintptr_t highestAddressPossible = highestAddressUsed + 0x100000000ULL;
#elif __arm__
	uintptr_t highestAddressPossible = 0x2fe00000;
#else
	uintptr_t highestAddressPossible = 0x80000000;
#endif
	uintptr_t sizeNeeded = highestAddressUsed-lowestAddressUsed;
	if ( (highestAddressPossible-sizeNeeded) < highestAddressUsed ) {
		// new and old segments will overlap 
		// need better algorithm for remapping
		// punt and don't re-map
		return orgMH;
	}
	uintptr_t possibleRange = (highestAddressPossible-sizeNeeded) - highestAddressUsed;
	uintptr_t newBaseAddress = highestAddressUsed + ((arc4random() % possibleRange) & -4096);
	
	vm_address_t addr = newBaseAddress;
	// reserve new address range
	if ( vm_allocate(mach_task_self(), &addr, sizeNeeded, VM_FLAGS_FIXED) == KERN_SUCCESS ) {
		// copy each segment to new address
		for (uint32_t i = 0; i < segCount; ++i) {
			uintptr_t newSegAddress = segs[i].vmaddr - lowestAddressUsed + newBaseAddress;
			if ( (vm_copy(mach_task_self(), segs[i].vmaddr, segs[i].vmsize, newSegAddress) != KERN_SUCCESS)
		#if !__arm__  // work around for <rdar://problem/5736393>
				|| (vm_protect(mach_task_self(), newSegAddress, segs[i].vmsize, true, segs[i].maxprot) != KERN_SUCCESS) 
		#endif
				|| (vm_protect(mach_task_self(), newSegAddress, segs[i].vmsize, false, segs[i].initprot) != KERN_SUCCESS) ) {
				// can't copy so dealloc new region and run with original base address
				vm_deallocate(mach_task_self(), newBaseAddress, sizeNeeded);
				dyld::warn("could not relocate position independent executable\n");
				return orgMH;
			}
		}
		// unmap original segments
		vm_deallocate(mach_task_self(), lowestAddressUsed, highestAddressUsed-lowestAddressUsed);
	
		// run with newly mapped executable
		*appsSlide = newBaseAddress - lowestAddressUsed;
		return (const struct macho_header*)newBaseAddress;
	}
	
	// can't get new range, so don't slide to random address
	return orgMH;
}


extern "C" void dyld_exceptions_init(const struct macho_header*, uintptr_t slide); // in dyldExceptions.cpp
extern "C" void mach_init();

//
// _pthread_keys is partitioned in a lower part that dyld will use; libSystem
// will use the upper part.  We set __pthread_tsd_first to 1 as the start of
// the lower part.  Libc will take #1 and c++ exceptions will take #2.  There
// is one free key=3 left.
//
extern "C" {
	extern int __pthread_tsd_first;
	extern void _pthread_keys_init();
}


//
//  This is code to bootstrap dyld.  This work in normally done for a program by dyld and crt.
//  In dyld we have to do this manually.
//
uintptr_t start(const struct macho_header* appsMachHeader, int argc, const char* argv[], intptr_t slide)
{
	// _mh_dylinker_header is magic symbol defined by static linker (ld), see <mach-o/ldsyms.h>
	const struct macho_header* dyldsMachHeader =  (const struct macho_header*)(((char*)&_mh_dylinker_header)+slide);
	
	// if kernel had to slide dyld, we need to fix up load sensitive locations
	// we have to do this before using any global variables
	if ( slide != 0 ) {
		rebaseDyld(dyldsMachHeader, slide);
	}
	
	uintptr_t appsSlide = 0;
		
	// enable C++ exceptions to work inside dyld
	dyld_exceptions_init(dyldsMachHeader, slide);
	
	// allow dyld to use mach messaging
	mach_init();

	// set protection on segments (has to be done after mach_init)
	segmentProtectDyld(dyldsMachHeader, slide);
	
	// kernel sets up env pointer to be just past end of agv array
	const char** envp = &argv[argc+1];
	
	// kernel sets up apple pointer to be just past end of envp array
	const char** apple = envp;
	while(*apple != NULL) { ++apple; }
	++apple;

	// run all C++ initializers inside dyld
	runDyldInitializers(dyldsMachHeader, slide, argc, argv, envp, apple);
	
	// if main executable was linked -pie, then randomize its load address
	if ( appsMachHeader->flags & MH_PIE )
		appsMachHeader = randomizeExecutableLoadAddress(appsMachHeader, envp, &appsSlide);
	
	// now that we are done bootstrapping dyld, call dyld's main
	return dyld::_main(appsMachHeader, appsSlide, argc, argv, envp, apple);
}




} // end of namespace




