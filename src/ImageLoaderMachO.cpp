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

// work around until conformance work is complete rdar://problem/4508801
#define __srr0	srr0 
#define __eip	eip 
#define __rip	rip 

#define __STDC_LIMIT_MACROS
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <mach-o/loader.h> 
#include <mach-o/nlist.h> 
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <System/sys/codesign.h>

#include "ImageLoaderMachO.h"
#include "ImageLoaderMachOCompressed.h"
#if SUPPORT_CLASSIC_MACHO
#include "ImageLoaderMachOClassic.h"
#endif
#include "mach-o/dyld_images.h"

// <rdar://problem/8718137> use stack guard random value to add padding between dylibs
extern "C" long __stack_chk_guard;

#ifndef LC_LOAD_UPWARD_DYLIB
	#define	LC_LOAD_UPWARD_DYLIB (0x23|LC_REQ_DYLD)	/* load of dylib whose initializers run later */
#endif

// relocation_info.r_length field has value 3 for 64-bit executables and value 2 for 32-bit executables
#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define LC_ROUTINES_COMMAND		LC_ROUTINES_64
	#define LC_SEGMENT_COMMAND_WRONG LC_SEGMENT
	struct macho_segment_command	: public segment_command_64  {};
	struct macho_section			: public section_64  {};	
	struct macho_routines_command	: public routines_command_64  {};	
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define LC_ROUTINES_COMMAND		LC_ROUTINES
	#define LC_SEGMENT_COMMAND_WRONG LC_SEGMENT_64
	struct macho_segment_command	: public segment_command {};
	struct macho_section			: public section  {};	
	struct macho_routines_command	: public routines_command  {};	
#endif

uint32_t ImageLoaderMachO::fgSymbolTableBinarySearchs = 0;
uint32_t ImageLoaderMachO::fgSymbolTrieSearchs = 0;


ImageLoaderMachO::ImageLoaderMachO(const macho_header* mh, const char* path, unsigned int segCount, 
																uint32_t segOffsets[], unsigned int libCount)
 : ImageLoader(path, libCount), fMachOData((uint8_t*)mh), fLinkEditBase(NULL), fSlide(0), 
	fEHFrameSectionOffset(0), fUnwindInfoSectionOffset(0), fDylibIDOffset(0), 
	fSegmentsCount(segCount), fIsSplitSeg(false), fInSharedCache(false),  
#if TEXT_RELOC_SUPPORT
	fTextSegmentRebases(false),
	fTextSegmentBinds(false),
#endif
#if __i386__
	fReadOnlyImportSegment(false),
#endif
	fHasSubLibraries(false), fHasSubUmbrella(false), fInUmbrella(false), fHasDOFSections(false), fHasDashInit(false),
	fHasInitializers(false), fHasTerminators(false), fRegisteredAsRequiresCoalescing(false)
{
	fIsSplitSeg = ((mh->flags & MH_SPLIT_SEGS) != 0);        

	// construct SegmentMachO object for each LC_SEGMENT cmd using "placement new" to put 
	// each SegmentMachO object in array at end of ImageLoaderMachO object
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0, segIndex=0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			// ignore zero-sized segments
			if ( segCmd->vmsize != 0 ) {
				// record offset of load command
				segOffsets[segIndex++] = (uint32_t)((uint8_t*)segCmd - fMachOData);
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}

}


// determine if this mach-o file has classic or compressed LINKEDIT and number of segments it has
void ImageLoaderMachO::sniffLoadCommands(const macho_header* mh, const char* path, bool* compressed, 
											unsigned int* segCount, unsigned int* libCount, const LinkContext& context,
											const linkedit_data_command** codeSigCmd)
{
	*compressed = false;
	*segCount = 0;
	*libCount = 0;
	*codeSigCmd = NULL;
	struct macho_segment_command* segCmd;
	bool foundLoadCommandSegment = false;
	uint32_t loadCommandSegmentIndex = 0xFFFFFFFF;
	uintptr_t loadCommandSegmentVMStart = 0;
	uintptr_t loadCommandSegmentVMEnd = 0;

	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const startCmds    = (struct load_command*)(((uint8_t*)mh) + sizeof(macho_header));
	const struct load_command* const endCmds = (struct load_command*)(((uint8_t*)mh) + sizeof(macho_header) + mh->sizeofcmds);
	const struct load_command* cmd = startCmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		uint32_t cmdLength = cmd->cmdsize;
		if ( cmdLength < 8 ) {
			dyld::throwf("malformed mach-o image: load command #%d length (%u) too small in %s",
											   i, cmdLength, path);
		}
		const struct load_command* const nextCmd = (const struct load_command*)(((char*)cmd)+cmdLength);
		if ( (nextCmd > endCmds) || (nextCmd < cmd) ) {
			dyld::throwf("malformed mach-o image: load command #%d length (%u) would exceed sizeofcmds (%u) in %s",
											   i, cmdLength, mh->sizeofcmds, path);
		}
		switch (cmd->cmd) {
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				*compressed = true;
				break;
			case LC_SEGMENT_COMMAND:
				segCmd = (struct macho_segment_command*)cmd;
				// ignore zero-sized segments
				if ( segCmd->vmsize != 0 )
					*segCount += 1;
				// <rdar://problem/7942521> all load commands must be in an executable segment
				if ( context.codeSigningEnforced && (segCmd->fileoff < mh->sizeofcmds) && (segCmd->filesize != 0) ) {
					if ( (segCmd->fileoff != 0) || (segCmd->filesize < (mh->sizeofcmds+sizeof(macho_header))) ) 
						dyld::throwf("malformed mach-o image: segment %s does not span all load commands", segCmd->segname); 
					if ( segCmd->initprot != (VM_PROT_READ | VM_PROT_EXECUTE) ) 
						dyld::throwf("malformed mach-o image: load commands found in segment %s with wrong permissions", segCmd->segname); 
					if ( foundLoadCommandSegment )
						throw "load commands in multiple segments";
					foundLoadCommandSegment = true;
					loadCommandSegmentIndex = i;
					loadCommandSegmentVMStart = segCmd->vmaddr;
					loadCommandSegmentVMEnd   = segCmd->vmaddr + segCmd->vmsize;
					if ( (intptr_t)(segCmd->vmsize) < 0)
						dyld::throwf("malformed mach-o image: segment load command %s size too large", segCmd->segname);
					if ( loadCommandSegmentVMEnd < loadCommandSegmentVMStart )
						dyld::throwf("malformed mach-o image: segment load command %s wraps around address space", segCmd->segname);
				}
				break;
			case LC_SEGMENT_COMMAND_WRONG:
				dyld::throwf("malformed mach-o image: wrong LC_SEGMENT[_64] for architecture"); 
				break;
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				*libCount += 1;
				break;
			case LC_CODE_SIGNATURE:
				*codeSigCmd = (struct linkedit_data_command*)cmd; // only support one LC_CODE_SIGNATURE per image
				break;
		}
		cmd = nextCmd;
	}
	
	if ( context.codeSigningEnforced && !foundLoadCommandSegment )
		throw "load commands not in a segment";
	// <rdar://problem/13145644> verify another segment does not over-map load commands
	cmd = startCmds;
	if ( context.codeSigningEnforced ) {
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_SEGMENT_COMMAND:
					if ( i != loadCommandSegmentIndex ) {
						segCmd = (struct macho_segment_command*)cmd;
						uintptr_t start = segCmd->vmaddr;
						uintptr_t end = segCmd->vmaddr + segCmd->vmsize;
						if ( ((start <= loadCommandSegmentVMStart) && (end > loadCommandSegmentVMStart)) 
						   || ((start >= loadCommandSegmentVMStart) && (start < loadCommandSegmentVMEnd)) )
							dyld::throwf("malformed mach-o image: segment %s overlaps load commands", segCmd->segname); 
					}
					break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
	
	// fSegmentsArrayCount is only 8-bits
	if ( *segCount > 255 )
		dyld::throwf("malformed mach-o image: more than 255 segments in %s", path);
		
	// fSegmentsArrayCount is only 8-bits
	if ( *libCount > 4095 )
		dyld::throwf("malformed mach-o image: more than 4095 dependent libraries in %s", path);

	if ( needsAddedLibSystemDepency(*libCount, mh) )
		*libCount = 1;
}



// create image for main executable
ImageLoader* ImageLoaderMachO::instantiateMainExecutable(const macho_header* mh, uintptr_t slide, const char* path, const LinkContext& context)
{
	//dyld::log("ImageLoader=%ld, ImageLoaderMachO=%ld, ImageLoaderMachOClassic=%ld, ImageLoaderMachOCompressed=%ld\n",
	//	sizeof(ImageLoader), sizeof(ImageLoaderMachO), sizeof(ImageLoaderMachOClassic), sizeof(ImageLoaderMachOCompressed));
	bool compressed;
	unsigned int segCount;
	unsigned int libCount;
	const linkedit_data_command* codeSigCmd;
	sniffLoadCommands(mh, path, &compressed, &segCount, &libCount, context, &codeSigCmd);
	// instantiate concrete class based on content of load commands
	if ( compressed ) 
		return ImageLoaderMachOCompressed::instantiateMainExecutable(mh, slide, path, segCount, libCount, context);
	else
#if SUPPORT_CLASSIC_MACHO
		return ImageLoaderMachOClassic::instantiateMainExecutable(mh, slide, path, segCount, libCount, context);
#else
		throw "missing LC_DYLD_INFO load command";
#endif
}


// create image by mapping in a mach-o file
ImageLoader* ImageLoaderMachO::instantiateFromFile(const char* path, int fd, const uint8_t firstPage[4096], uint64_t offsetInFat, 
									uint64_t lenInFat, const struct stat& info, const LinkContext& context)
{
	// get load commands
	const unsigned int dataSize = sizeof(macho_header) + ((macho_header*)firstPage)->sizeofcmds;
	uint8_t buffer[dataSize];
	const uint8_t* fileData = firstPage;
	if ( dataSize > 4096 ) {
		// only read more if cmds take up more space than first page
		fileData = buffer;
		memcpy(buffer, firstPage, 4096);
		pread(fd, &buffer[4096], dataSize-4096, offsetInFat+4096);
	}

	bool compressed;
	unsigned int segCount;
	unsigned int libCount;
	const linkedit_data_command* codeSigCmd;
	sniffLoadCommands((const macho_header*)fileData, path, &compressed, &segCount, &libCount, context, &codeSigCmd);
	// instantiate concrete class based on content of load commands
	if ( compressed ) 
		return ImageLoaderMachOCompressed::instantiateFromFile(path, fd, fileData, offsetInFat, lenInFat, info, segCount, libCount, codeSigCmd, context);
	else
#if SUPPORT_CLASSIC_MACHO
		return ImageLoaderMachOClassic::instantiateFromFile(path, fd, fileData, offsetInFat, lenInFat, info, segCount, libCount, codeSigCmd, context);
#else
		throw "missing LC_DYLD_INFO load command";
#endif
}

// create image by using cached mach-o file
ImageLoader* ImageLoaderMachO::instantiateFromCache(const macho_header* mh, const char* path, long slide, const struct stat& info, const LinkContext& context)
{
	// instantiate right concrete class
	bool compressed;
	unsigned int segCount;
	unsigned int libCount;
	const linkedit_data_command* codeSigCmd;
	sniffLoadCommands(mh, path, &compressed, &segCount, &libCount, context, &codeSigCmd);
	// instantiate concrete class based on content of load commands
	if ( compressed ) 
		return ImageLoaderMachOCompressed::instantiateFromCache(mh, path, slide, info, segCount, libCount, context);
	else
#if SUPPORT_CLASSIC_MACHO
		return ImageLoaderMachOClassic::instantiateFromCache(mh, path, slide, info, segCount, libCount, context);
#else
		throw "missing LC_DYLD_INFO load command";
#endif
}

// create image by copying an in-memory mach-o file
ImageLoader* ImageLoaderMachO::instantiateFromMemory(const char* moduleName, const macho_header* mh, uint64_t len, const LinkContext& context)
{
	bool compressed;
	unsigned int segCount;
	unsigned int libCount;
	const linkedit_data_command* sigcmd;
	sniffLoadCommands(mh, moduleName, &compressed, &segCount, &libCount, context, &sigcmd);
	// instantiate concrete class based on content of load commands
	if ( compressed ) 
		return ImageLoaderMachOCompressed::instantiateFromMemory(moduleName, mh, len, segCount, libCount, context);
	else
#if SUPPORT_CLASSIC_MACHO
		return ImageLoaderMachOClassic::instantiateFromMemory(moduleName, mh, len, segCount, libCount, context);
#else
		throw "missing LC_DYLD_INFO load command";
#endif
}


int ImageLoaderMachO::crashIfInvalidCodeSignature()
{
	// Now that segments are mapped in, try reading from first executable segment.
	// If code signing is enabled the kernel will validate the code signature
	// when paging in, and kill the process if invalid.
	for(unsigned int i=0; i < fSegmentsCount; ++i) {
		if ( (segFileOffset(i) == 0) && (segFileSize(i) != 0) ) {
			// return read value to ensure compiler does not optimize away load
			int* p = (int*)segActualLoadAddress(i);
			return *p;
		}
	}
	return 0;
}


void ImageLoaderMachO::parseLoadCmds()
{
	// now that segments are mapped in, get real fMachOData, fLinkEditBase, and fSlide
	for(unsigned int i=0; i < fSegmentsCount; ++i) {
		// set up pointer to __LINKEDIT segment
		if ( strcmp(segName(i),"__LINKEDIT") == 0 ) 
			fLinkEditBase = (uint8_t*)(segActualLoadAddress(i) - segFileOffset(i));
#if TEXT_RELOC_SUPPORT
		// __TEXT segment always starts at beginning of file and contains mach_header and load commands
		if ( segExecutable(i) ) {
			if ( segHasRebaseFixUps(i) && (fSlide != 0) )
				fTextSegmentRebases = true;
			if ( segHasBindFixUps(i) )
				fTextSegmentBinds = true;
		}
#endif
#if __i386__
		if ( segIsReadOnlyImport(i) )
			fReadOnlyImportSegment = true;
#endif
		// some segment always starts at beginning of file and contains mach_header and load commands
		if ( (segFileOffset(i) == 0) && (segFileSize(i) != 0) ) {
			fMachOData = (uint8_t*)(segActualLoadAddress(i));
		}
	}
	
	// keep count of prebound images with weak exports
	if ( this->participatesInCoalescing() ) {
		++fgImagesRequiringCoalescing;
		fRegisteredAsRequiresCoalescing = true;
		if ( this->hasCoalescedExports() ) 
			++fgImagesHasWeakDefinitions;
	}

	// keep count of images used in shared cache
	if ( fInSharedCache )
		++fgImagesUsedFromSharedCache;

	// walk load commands (mapped in at start of __TEXT segment)
	const dyld_info_command* dyldInfo = NULL;
	const macho_nlist* symbolTable = NULL;
	const char* symbolTableStrings = NULL;
	const struct load_command* firstUnknownCmd = NULL;
	const struct version_min_command* minOSVersionCmd = NULL;
	const dysymtab_command* dynSymbolTable = NULL;
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SYMTAB:
				{
					const struct symtab_command* symtab = (struct symtab_command*)cmd;
					symbolTableStrings = (const char*)&fLinkEditBase[symtab->stroff];
					symbolTable = (macho_nlist*)(&fLinkEditBase[symtab->symoff]);
				}
				break;
			case LC_DYSYMTAB:
				dynSymbolTable = (struct dysymtab_command*)cmd;
				break;
			case LC_SUB_UMBRELLA:
				fHasSubUmbrella = true;
				break;
			case LC_SUB_FRAMEWORK:
				fInUmbrella = true;
				break;
			case LC_SUB_LIBRARY:
				fHasSubLibraries = true;
				break;
			case LC_ROUTINES_COMMAND:
				fHasDashInit = true;
				break;
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				dyldInfo = (struct dyld_info_command*)cmd;
				break;
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const bool isTextSeg = (strcmp(seg->segname, "__TEXT") == 0);
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_MOD_INIT_FUNC_POINTERS )
							fHasInitializers = true;
						else if ( type == S_MOD_TERM_FUNC_POINTERS )
							fHasTerminators = true;
						else if ( type == S_DTRACE_DOF )
							fHasDOFSections = true;
						else if ( isTextSeg && (strcmp(sect->sectname, "__eh_frame") == 0) )
							fEHFrameSectionOffset = (uint32_t)((uint8_t*)sect - fMachOData);
						else if ( isTextSeg && (strcmp(sect->sectname, "__unwind_info") == 0) )
							fUnwindInfoSectionOffset = (uint32_t)((uint8_t*)sect - fMachOData);
					}
				}
				break;
			case LC_TWOLEVEL_HINTS:
				// no longer supported
				break;
			case LC_ID_DYLIB:
				{
					fDylibIDOffset = (uint32_t)((uint8_t*)cmd - fMachOData);
				}
				break;
			case LC_RPATH:
			case LC_LOAD_WEAK_DYLIB:
		    case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
			case LC_MAIN:
				// do nothing, just prevent LC_REQ_DYLD exception from occuring
				break;
			case LC_VERSION_MIN_MACOSX:
			case LC_VERSION_MIN_IPHONEOS:
				minOSVersionCmd = (version_min_command*)cmd;
				break;
			default:
				if ( (cmd->cmd & LC_REQ_DYLD) != 0 ) {
					if ( firstUnknownCmd == NULL )
						firstUnknownCmd = cmd;
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	if ( firstUnknownCmd != NULL ) {
		if ( minOSVersionCmd != NULL )  {
			dyld::throwf("cannot load '%s' because it was built for OS version %u.%u (load command 0x%08X is unknown)", 
						 this->getShortName(),
						 minOSVersionCmd->version >> 16, ((minOSVersionCmd->version >> 8) & 0xff), 
						 firstUnknownCmd->cmd);
		}
		else {
			dyld::throwf("cannot load '%s' (load command 0x%08X is unknown)", this->getShortName(), firstUnknownCmd->cmd);
		}
	}
	
	
	if ( dyldInfo != NULL )
		this->setDyldInfo(dyldInfo);
	if ( symbolTable != NULL)
		this->setSymbolTableInfo(symbolTable, symbolTableStrings, dynSymbolTable);
	
}

// don't do this work in destructor because we need object to be full subclass
// for UnmapSegments() to work
void ImageLoaderMachO::destroy()
{
	// update count of images with weak exports
	if ( fRegisteredAsRequiresCoalescing ) {
		--fgImagesRequiringCoalescing;
		if ( this->hasCoalescedExports() ) 
			--fgImagesHasWeakDefinitions;
	}

	// keep count of images used in shared cache
	if ( fInSharedCache )
		--fgImagesUsedFromSharedCache;
		
	// unmap image when done
	UnmapSegments();
}


unsigned int ImageLoaderMachO::segmentCount() const
{
	return fSegmentsCount;
}


const macho_segment_command* ImageLoaderMachO::segLoadCommand(unsigned int segIndex) const
{
	uint32_t* lcOffsets = this->segmentCommandOffsets();
	uint32_t lcOffset =	lcOffsets[segIndex];
	return (macho_segment_command*)(&fMachOData[lcOffset]);
}

const char*	ImageLoaderMachO::segName(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->segname;
}


uintptr_t ImageLoaderMachO::segSize(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->vmsize;
}


uintptr_t ImageLoaderMachO::segFileSize(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->filesize;
}


bool ImageLoaderMachO::segHasTrailingZeroFill(unsigned int segIndex)
{
	return ( segWriteable(segIndex) && (segSize(segIndex) > segFileSize(segIndex)) );
}


uintptr_t ImageLoaderMachO::segFileOffset(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->fileoff;
}


bool ImageLoaderMachO::segReadable(unsigned int segIndex) const
{
	return ( (segLoadCommand(segIndex)->initprot & VM_PROT_READ) != 0);
}


bool ImageLoaderMachO::segWriteable(unsigned int segIndex) const
{
	return ( (segLoadCommand(segIndex)->initprot & VM_PROT_WRITE) != 0);
}


bool ImageLoaderMachO::segExecutable(unsigned int segIndex) const
{
	return ( (segLoadCommand(segIndex)->initprot & VM_PROT_EXECUTE) != 0);
}


bool ImageLoaderMachO::segUnaccessible(unsigned int segIndex) const
{
	return (segLoadCommand(segIndex)->initprot == 0);
}

bool ImageLoaderMachO::segHasPreferredLoadAddress(unsigned int segIndex) const
{
	return (segLoadCommand(segIndex)->vmaddr != 0);
}

uintptr_t ImageLoaderMachO::segPreferredLoadAddress(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->vmaddr;
}

uintptr_t ImageLoaderMachO::segActualLoadAddress(unsigned int segIndex) const
{
	return segLoadCommand(segIndex)->vmaddr + fSlide;
}


uintptr_t ImageLoaderMachO::segActualEndAddress(unsigned int segIndex) const
{
	return segActualLoadAddress(segIndex) + segSize(segIndex);
}

bool ImageLoaderMachO::segHasRebaseFixUps(unsigned int segIndex) const
{
#if TEXT_RELOC_SUPPORT
	// scan sections for fix-up bit
	const macho_segment_command* segCmd = segLoadCommand(segIndex);
	const struct macho_section* const sectionsStart = (struct macho_section*)((char*)segCmd + sizeof(struct macho_segment_command));
	const struct macho_section* const sectionsEnd = &sectionsStart[segCmd->nsects];
	for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
		if ( (sect->flags & S_ATTR_LOC_RELOC) != 0 )
			return true;
	}
#endif
	return false;
}

bool ImageLoaderMachO::segHasBindFixUps(unsigned int segIndex) const
{
#if TEXT_RELOC_SUPPORT
	// scan sections for fix-up bit
	const macho_segment_command* segCmd = segLoadCommand(segIndex);
	const struct macho_section* const sectionsStart = (struct macho_section*)((char*)segCmd + sizeof(struct macho_segment_command));
	const struct macho_section* const sectionsEnd = &sectionsStart[segCmd->nsects];
	for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
		if ( (sect->flags & S_ATTR_EXT_RELOC) != 0 )
			return true;
	}
#endif
	return false;
}

#if __i386__
bool ImageLoaderMachO::segIsReadOnlyImport(unsigned int segIndex) const
{
	const macho_segment_command* segCmd = segLoadCommand(segIndex);
	return (    (segCmd->initprot & VM_PROT_EXECUTE) 
			&& ((segCmd->initprot & VM_PROT_WRITE) == 0) 
			&& (strcmp(segCmd->segname, "__IMPORT") == 0) );
}
#endif


void ImageLoaderMachO::UnmapSegments()
{
	// usually unmap image when done
	if ( ! this->leaveMapped() && (this->getState() >= dyld_image_state_mapped) ) {
		// unmap TEXT segment last because it contains load command being inspected
		unsigned int textSegmentIndex = 0;
		for(unsigned int i=0; i < fSegmentsCount; ++i) {
			//dyld::log("unmap %s at 0x%08lX\n", seg->getName(), seg->getActualLoadAddress(this));
			if ( strcmp(segName(i), "__TEXT") == 0 ) {
				textSegmentIndex = i;
			}
			else {
				// update stats
				--ImageLoader::fgTotalSegmentsMapped;
				ImageLoader::fgTotalBytesMapped -= segSize(i);
				munmap((void*)segActualLoadAddress(i), segSize(i));
			}
		}
		// now unmap TEXT
		--ImageLoader::fgTotalSegmentsMapped;
		ImageLoader::fgTotalBytesMapped -= segSize(textSegmentIndex);
		munmap((void*)segActualLoadAddress(textSegmentIndex), segSize(textSegmentIndex));
	}
}


// prefetch __DATA/__OBJC pages during launch, but not for dynamically loaded code
void ImageLoaderMachO::preFetchDATA(int fd, uint64_t offsetInFat, const LinkContext& context)
{
	if ( context.linkingMainExecutable ) {
		for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
			if ( segWriteable(i) && (segFileSize(i) > 0) ) {
				// prefetch writable segment that have mmap'ed regions
				radvisory advice;
				advice.ra_offset = offsetInFat + segFileOffset(i);
				advice.ra_count = (int)segFileSize(i);
				// limit prefetch to 1MB (256 pages)
				if ( advice.ra_count > 1024*1024 )
					advice.ra_count = 1024*1024;
				// don't prefetch single pages, let them fault in
				fgTotalBytesPreFetched += advice.ra_count;
				fcntl(fd, F_RDADVISE, &advice);
				if ( context.verboseMapping ) {
					dyld::log("%18s prefetching 0x%0lX -> 0x%0lX\n", 
						segName(i), segActualLoadAddress(i), segActualLoadAddress(i)+advice.ra_count-1);
				}
			}
		}
	}
}


bool ImageLoaderMachO::segmentsMustSlideTogether() const 
{
	return true;
}

bool ImageLoaderMachO::segmentsCanSlide() const 
{
	return (this->isDylib() || this->isBundle() || this->isPositionIndependentExecutable());
}

bool ImageLoaderMachO::isBundle() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( mh->filetype == MH_BUNDLE );
}

bool ImageLoaderMachO::isDylib() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( mh->filetype == MH_DYLIB );
}

bool ImageLoaderMachO::isExecutable() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( mh->filetype == MH_EXECUTE );
}

bool ImageLoaderMachO::isPositionIndependentExecutable() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->filetype == MH_EXECUTE) && ((mh->flags & MH_PIE) != 0) );
}


bool ImageLoaderMachO::forceFlat() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_FORCE_FLAT) != 0 );
}

bool ImageLoaderMachO::usesTwoLevelNameSpace() const
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_TWOLEVEL) != 0 );
}

bool ImageLoaderMachO::isPrebindable() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_PREBOUND) != 0 );
}

bool ImageLoaderMachO::hasCoalescedExports() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_WEAK_DEFINES) != 0 );
}

bool ImageLoaderMachO::hasReferencesToWeakSymbols() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_BINDS_TO_WEAK) != 0 );
}

bool ImageLoaderMachO::participatesInCoalescing() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	// if image is loaded with RTLD_LOCAL, then its symbols' visibility
	// is reduced and it can't coalesce with other images
	if ( this->hasHiddenExports() )
		return false;
	return ( (mh->flags & (MH_WEAK_DEFINES|MH_BINDS_TO_WEAK)) != 0 );
}



void ImageLoaderMachO::setSlide(intptr_t slide)
{
	fSlide = slide;
}

void ImageLoaderMachO::loadCodeSignature(const struct linkedit_data_command* codeSigCmd, int fd,  uint64_t offsetInFatFile, const LinkContext& context)
{
	// if dylib being loaded has no code signature load command
	if ( codeSigCmd == NULL ) {
#if __MAC_OS_X_VERSION_MIN_REQUIRED
		bool codeSigningEnforced = context.codeSigningEnforced;
		if ( context.mainExecutableCodeSigned && !codeSigningEnforced ) {
			static bool codeSignEnforcementDynamicallyEnabled = false;
			if ( !codeSignEnforcementDynamicallyEnabled ) {
				uint32_t flags;
				if ( csops(0, CS_OPS_STATUS, &flags, sizeof(flags)) != -1 ) {
					if ( flags & CS_ENFORCEMENT ) {
						codeSignEnforcementDynamicallyEnabled = true;
					}
				}
			}
			codeSigningEnforced = codeSignEnforcementDynamicallyEnabled;
		}
		// if we require dylibs to be code signed
		if ( codeSigningEnforced  ) {
			// if there is a non-load command based code signature, use it
			off_t offset = (off_t)offsetInFatFile;
			if ( fcntl(fd, F_FINDSIGS, &offset, sizeof(offset)) != -1 )
				return;
			// otherwise gracefully return from dlopen()
			dyld::throwf("required code signature missing for '%s'\n", this->getPath());
		}
#endif
	}
	else {
#if __MAC_OS_X_VERSION_MIN_REQUIRED
		// <rdar://problem/13622786> ignore code signatures in binaries built with pre-10.9 tools
		if ( this->sdkVersion() < DYLD_MACOSX_VERSION_10_9 ) {
			return;
		}
#endif
		fsignatures_t siginfo;
		siginfo.fs_file_start=offsetInFatFile;				// start of mach-o slice in fat file 
		siginfo.fs_blob_start=(void*)(long)(codeSigCmd->dataoff);	// start of CD in mach-o file
		siginfo.fs_blob_size=codeSigCmd->datasize;			// size of CD
		int result = fcntl(fd, F_ADDFILESIGS, &siginfo);
		if ( result == -1 ) {
			if ( (errno == EPERM) || (errno == EBADEXEC) )
				dyld::throwf("code signature invalid for '%s'\n", this->getPath());
			if ( context.verboseCodeSignatures ) 
				dyld::log("dyld: Failed registering code signature for %s, errno=%d\n", this->getPath(), errno);
			else
				dyld::log("dyld: Registered code signature for %s\n", this->getPath());
		}
	}
}


const char* ImageLoaderMachO::getInstallPath() const
{
	if ( fDylibIDOffset != 0 ) {
		const dylib_command* dylibID = (dylib_command*)(&fMachOData[fDylibIDOffset]);
		return (char*)dylibID + dylibID->dylib.name.offset;
	}
	return NULL;
}

void ImageLoaderMachO::registerInterposing()
{
	// mach-o files advertise interposing by having a __DATA __interpose section
	struct InterposeData { uintptr_t replacement; uintptr_t replacee; };
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( ((sect->flags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(sect->sectname, "__interpose") == 0) && (strcmp(seg->segname, "__DATA") == 0)) ) {
							const InterposeData* interposeArray = (InterposeData*)(sect->addr + fSlide);
							const size_t count = sect->size / sizeof(InterposeData);
							for (size_t i=0; i < count; ++i) {
								ImageLoader::InterposeTuple tuple;
								tuple.replacement		= interposeArray[i].replacement;
								tuple.neverImage		= this;
								tuple.onlyImage		    = NULL;
								tuple.replacee			= interposeArray[i].replacee;
								// <rdar://problem/7937695> verify that replacement is in this image
								if ( this->containsAddress((void*)tuple.replacement) ) {
									// chain to any existing interpositions
									for (std::vector<InterposeTuple>::iterator it=fgInterposingTuples.begin(); it != fgInterposingTuples.end(); it++) {
										if ( it->replacee == tuple.replacee ) {
											tuple.replacee = it->replacement;
										}
									}
									ImageLoader::fgInterposingTuples.push_back(tuple);
								}
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

uint32_t ImageLoaderMachO::sdkVersion() const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	const struct version_min_command* versCmd;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch ( cmd->cmd ) {
			case LC_VERSION_MIN_MACOSX:
			case LC_VERSION_MIN_IPHONEOS:
				versCmd = (version_min_command*)cmd;
				return versCmd->sdk;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return 0;
}

uint32_t ImageLoaderMachO::minOSVersion(const mach_header* mh)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh) + sizeof(macho_header));
	const struct load_command* cmd = cmds;
	const struct version_min_command* versCmd;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch ( cmd->cmd ) {
			case LC_VERSION_MIN_MACOSX:
			case LC_VERSION_MIN_IPHONEOS:
				versCmd = (version_min_command*)cmd;
				return versCmd->version;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return 0;
}

uint32_t ImageLoaderMachO::minOSVersion() const
{
	return ImageLoaderMachO::minOSVersion(machHeader());
}


void* ImageLoaderMachO::getThreadPC() const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_MAIN ) {
			entry_point_command* mainCmd = (entry_point_command*)cmd;
			void* entry = (void*)(mainCmd->entryoff + (char*)fMachOData);
			// <rdar://problem/8543820&9228031> verify entry point is in image
			if ( this->containsAddress(entry) )
				return entry;
			else
				throw "LC_MAIN entryoff is out of range";
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return NULL;
}


void* ImageLoaderMachO::getMain() const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_UNIXTHREAD:
			{
			#if __i386__
				const i386_thread_state_t* registers = (i386_thread_state_t*)(((char*)cmd) + 16);
				void* entry = (void*)(registers->eip + fSlide);
			#elif __x86_64__
				const x86_thread_state64_t* registers = (x86_thread_state64_t*)(((char*)cmd) + 16);
				void* entry = (void*)(registers->rip + fSlide);
			#elif __arm__
				const arm_thread_state_t* registers = (arm_thread_state_t*)(((char*)cmd) + 16);
				void* entry = (void*)(registers->__pc + fSlide);
			#elif __arm64__
				const arm_thread_state64_t* registers = (arm_thread_state64_t*)(((char*)cmd) + 16);
				void* entry = (void*)(registers->__pc + fSlide);
			#else
				#warning need processor specific code
			#endif
				// <rdar://problem/8543820&9228031> verify entry point is in image
				if ( this->containsAddress(entry) ) {
					return entry;
				}
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	throw "no valid entry point";
}

bool ImageLoaderMachO::needsAddedLibSystemDepency(unsigned int libCount, const macho_header* mh)
{
	// <rdar://problem/6357561> ensure that every image depends on something which depends on libSystem
	if ( libCount > 1 )
		return false;
		
	// <rdar://problem/6409800> dyld implicit-libSystem breaks valgrind
	if ( mh->filetype == MH_EXECUTE ) 
		return false;
	
	bool isNonOSdylib = false;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)((uint8_t*)mh+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				return false;
			case LC_ID_DYLIB:
				{
				const dylib_command* dylibID = (dylib_command*)cmd;
				const char* installPath = (char*)cmd + dylibID->dylib.name.offset;
				// It is OK for OS dylibs (libSystem or libmath or Rosetta shims) to have no dependents
				// but all other dylibs must depend on libSystem for initialization to initialize libSystem first
				// <rdar://problem/6497528> rosetta circular dependency spew
				isNonOSdylib = ( (strncmp(installPath, "/usr/lib/", 9) != 0) && (strncmp(installPath, "/usr/libexec/oah/Shims", 9) != 0) );
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return isNonOSdylib;
}


void ImageLoaderMachO::doGetDependentLibraries(DependentLibraryInfo libs[])
{
	if ( needsAddedLibSystemDepency(libraryCount(), (macho_header*)fMachOData) ) {
		DependentLibraryInfo* lib = &libs[0];
		lib->name = "/usr/lib/libSystem.B.dylib";
		lib->info.checksum = 0;
		lib->info.minVersion = 0;
		lib->info.maxVersion = 0;
		lib->required = false;
		lib->reExported = false;
		lib->upward = false;
	}
	else {
		uint32_t index = 0;
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_LOAD_DYLIB:
				case LC_LOAD_WEAK_DYLIB:
				case LC_REEXPORT_DYLIB:
				case LC_LOAD_UPWARD_DYLIB:
				{
					const struct dylib_command* dylib = (struct dylib_command*)cmd;
					DependentLibraryInfo* lib = &libs[index++];
					lib->name = (char*)cmd + dylib->dylib.name.offset;
					//lib->name = strdup((char*)cmd + dylib->dylib.name.offset);
					lib->info.checksum = dylib->dylib.timestamp;
					lib->info.minVersion = dylib->dylib.compatibility_version;
					lib->info.maxVersion = dylib->dylib.current_version;
					lib->required = (cmd->cmd != LC_LOAD_WEAK_DYLIB);
					lib->reExported = (cmd->cmd == LC_REEXPORT_DYLIB);
					lib->upward = (cmd->cmd == LC_LOAD_UPWARD_DYLIB);
				}
				break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}

ImageLoader::LibraryInfo ImageLoaderMachO::doGetLibraryInfo()
{
	LibraryInfo info;
	if ( fDylibIDOffset != 0 ) {
		const dylib_command* dylibID = (dylib_command*)(&fMachOData[fDylibIDOffset]);
		info.minVersion = dylibID->dylib.compatibility_version;
		info.maxVersion = dylibID->dylib.current_version;
		info.checksum = dylibID->dylib.timestamp;
	}
	else {
		info.minVersion = 0;
		info.maxVersion = 0;		
		info.checksum = 0;
	}
	return info;
}

void ImageLoaderMachO::getRPaths(const LinkContext& context, std::vector<const char*>& paths) const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_RPATH:
				const char* pathToAdd = NULL;
				const char* path = (char*)cmd + ((struct rpath_command*)cmd)->path.offset;
				if ( (strncmp(path, "@loader_path", 12) == 0) && ((path[12] == '/') || (path[12] == '\0')) ) {
					if ( context.processIsRestricted  && (context.mainExecutable == this) ) {
						dyld::warn("LC_RPATH %s in %s being ignored in restricted program because of @loader_path\n", path, this->getPath());
						break;
					}
					char resolvedPath[PATH_MAX];
					if ( realpath(this->getPath(), resolvedPath) != NULL ) {
						char newRealPath[strlen(resolvedPath) + strlen(path)];
						strcpy(newRealPath, resolvedPath);
						char* addPoint = strrchr(newRealPath,'/');
						if ( addPoint != NULL ) {
							strcpy(addPoint, &path[12]);
							pathToAdd = strdup(newRealPath);
						}
					}
				}
				else if ( (strncmp(path, "@executable_path", 16) == 0) && ((path[16] == '/') || (path[16] == '\0')) ) {
					if ( context.processIsRestricted ) {
						dyld::warn("LC_RPATH %s in %s being ignored in restricted program because of @executable_path\n", path, this->getPath());
						break;
					}
					char resolvedPath[PATH_MAX];
					if ( realpath(context.mainExecutable->getPath(), resolvedPath) != NULL ) {
						char newRealPath[strlen(resolvedPath) + strlen(path)];
						strcpy(newRealPath, resolvedPath);
						char* addPoint = strrchr(newRealPath,'/');
						if ( addPoint != NULL ) {
							strcpy(addPoint, &path[16]);
							pathToAdd = strdup(newRealPath);
						}
					}
				}
				else if ( (path[0] != '/') && context.processIsRestricted ) {
					dyld::warn("LC_RPATH %s in %s being ignored in restricted program because it is a relative path\n", path, this->getPath());
					break;
				}
				else if ( (path[0] == '/') && (context.rootPaths != NULL) ) {
					// <rdar://problem/5869973> DYLD_ROOT_PATH should apply to LC_RPATH rpaths
					// DYLD_ROOT_PATH can be a list of paths, but at this point we can only support one, so use first combination that exists
					bool found = false;
					for(const char** rp = context.rootPaths; *rp != NULL; ++rp) {
						char newPath[PATH_MAX];
						strlcpy(newPath, *rp, PATH_MAX);
						strlcat(newPath, path, PATH_MAX);
						struct stat stat_buf;
						if ( stat(newPath, &stat_buf) != -1 ) {
							//dyld::log("combined DYLD_ROOT_PATH and LC_RPATH: %s\n", newPath);
							pathToAdd = strdup(newPath);
							found = true;
							break;
						}
					}
					if ( ! found ) {
						// make copy so that all elements of 'paths' can be freed
						pathToAdd = strdup(path);
					}
				}
				else {
					// make copy so that all elements of 'paths' can be freed
					pathToAdd = strdup(path);
				}
				if ( pathToAdd != NULL )
					paths.push_back(pathToAdd);
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}


bool ImageLoaderMachO::getUUID(uuid_t uuid) const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_UUID:
				uuid_command* uc = (uuid_command*)cmd;
				memcpy(uuid, uc->uuid, 16);
				return true;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	bzero(uuid, 16);
	return false;
}

void ImageLoaderMachO::doRebase(const LinkContext& context)
{
	// if prebound and loaded at prebound address, then no need to rebase
	if ( this->usablePrebinding(context) ) {
		// skip rebasing because prebinding is valid
		++fgImagesWithUsedPrebinding; // bump totals for statistics
		return;
	}

	// print why prebinding was not used
	if ( context.verbosePrebinding ) {
		if ( !this->isPrebindable() ) {
			dyld::log("dyld: image not prebound, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( fSlide != 0 ) {
			dyld::log("dyld: image slid, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->allDependentLibrariesAsWhenPreBound() ) {
			dyld::log("dyld: dependent libraries changed, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->usesTwoLevelNameSpace() ){
			dyld::log("dyld: image uses flat-namespace so, parts of prebinding ignored %s\n", this->getPath());
		}
		else {
			dyld::log("dyld: environment variable disabled use of prebinding in %s\n", this->getPath());
		}
	}

	//dyld::log("slide=0x%08lX for %s\n", slide, this->getPath());

#if PREBOUND_IMAGE_SUPPORT
	// if prebound and we got here, then prebinding is not valid, so reset all lazy pointers
	// if this image is in the shared cache, do not reset, they will be bound in doBind()
	if ( this->isPrebindable() && !fInSharedCache )
		this->resetPreboundLazyPointers(context);
#endif

	// if loaded at preferred address, no rebasing necessary
	if ( this->fSlide == 0 ) 
		return;

#if TEXT_RELOC_SUPPORT
	// if there are __TEXT fixups, temporarily make __TEXT writable
	if ( fTextSegmentRebases ) 
		this->makeTextSegmentWritable(context, true);
#endif

	// do actual rebasing
	this->rebase(context);
			
#if TEXT_RELOC_SUPPORT
	// if there were __TEXT fixups, restore write protection
	if ( fTextSegmentRebases ) 
		this->makeTextSegmentWritable(context, false);
	
#endif	
}

#if TEXT_RELOC_SUPPORT
void ImageLoaderMachO::makeTextSegmentWritable(const LinkContext& context, bool writeable)
{
	for(unsigned int i=0; i < fSegmentsCount; ++i) {
		if ( segExecutable(i) ) {
			if ( writeable ) {
				segMakeWritable(i, context);
			}
			else {
			#if !__i386__ && !__x86_64__
				// some processors require range to be invalidated before it is made executable
				sys_icache_invalidate((void*)segActualLoadAddress(i), segSize(textSegmentIndex));
			#endif
				segProtect(i, context);
			}
		}
	}

}
#endif

const ImageLoader::Symbol* ImageLoaderMachO::findExportedSymbol(const char* name, bool searchReExports, const ImageLoader** foundIn) const
{
	// look in this image first
	const ImageLoader::Symbol* result = this->findExportedSymbol(name, foundIn);
	if ( result != NULL )
		return result;
	
	if ( searchReExports ) {
		for(unsigned int i=0; i < libraryCount(); ++i){
			if ( libReExported(i) ) {
				ImageLoader* image = libImage(i);
				if ( image != NULL ) {
					const Symbol* result = image->findExportedSymbol(name, searchReExports, foundIn);
					if ( result != NULL )
						return result;
				}
			}
		}
	}
	

	return NULL;
}



uintptr_t ImageLoaderMachO::getExportedSymbolAddress(const Symbol* sym, const LinkContext& context, 
											const ImageLoader* requestor, bool runResolver) const
{
	return this->getSymbolAddress(sym, requestor, context, runResolver);
}

uintptr_t ImageLoaderMachO::getSymbolAddress(const Symbol* sym, const ImageLoader* requestor, 
												const LinkContext& context, bool runResolver) const
{
	uintptr_t result = exportedSymbolAddress(context, sym, requestor, runResolver);
	// check for interposing overrides
	result = interposedAddress(context, result, requestor);
	return result;
}

ImageLoader::DefinitionFlags ImageLoaderMachO::getExportedSymbolInfo(const Symbol* sym) const
{
	if ( exportedSymbolIsWeakDefintion(sym) )
		return kWeakDefinition;
	else
		return kNoDefinitionOptions;
}

const char* ImageLoaderMachO::getExportedSymbolName(const Symbol* sym) const
{
	return exportedSymbolName(sym);
}

uint32_t ImageLoaderMachO::getExportedSymbolCount() const
{
	return exportedSymbolCount();
}


const ImageLoader::Symbol* ImageLoaderMachO::getIndexedExportedSymbol(uint32_t index) const
{
	return exportedSymbolIndexed(index);
}


uint32_t ImageLoaderMachO::getImportedSymbolCount() const
{
	return importedSymbolCount();
}


const ImageLoader::Symbol* ImageLoaderMachO::getIndexedImportedSymbol(uint32_t index) const
{
	return importedSymbolIndexed(index);
}


ImageLoader::ReferenceFlags ImageLoaderMachO::getImportedSymbolInfo(const ImageLoader::Symbol* sym) const
{
	ImageLoader::ReferenceFlags flags = kNoReferenceOptions;
	return flags;
}


const char* ImageLoaderMachO::getImportedSymbolName(const ImageLoader::Symbol* sym) const
{
	return importedSymbolName(sym);
}


bool ImageLoaderMachO::getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length)
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( (strcmp(sect->segname, segmentName) == 0) && (strcmp(sect->sectname, sectionName) == 0) ) {
							*start = (uintptr_t*)(sect->addr + fSlide);
							*length = sect->size;
							return true;
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	*start = NULL;
	*length = 0;
	return false;
}

void ImageLoaderMachO::getUnwindInfo(dyld_unwind_sections* info)
{
	info->mh = this->machHeader();
	info->dwarf_section = 0;
	info->dwarf_section_length = 0;
	info->compact_unwind_section = 0;
	info->compact_unwind_section_length = 0;
	if ( fEHFrameSectionOffset != 0 ) {
		const macho_section* sect = (macho_section*)&fMachOData[fEHFrameSectionOffset];
		info->dwarf_section = (void*)(sect->addr + fSlide);
		info->dwarf_section_length = sect->size;
	}
	if ( fUnwindInfoSectionOffset != 0 ) {
		const macho_section* sect = (macho_section*)&fMachOData[fUnwindInfoSectionOffset];
		info->compact_unwind_section = (void*)(sect->addr + fSlide);
		info->compact_unwind_section_length = sect->size;
	}
}


bool ImageLoaderMachO::findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset)
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	const uintptr_t unslidInteriorAddress = (uintptr_t)imageInterior - this->getSlide();
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					if ( (unslidInteriorAddress >= seg->vmaddr) && (unslidInteriorAddress < (seg->vmaddr+seg->vmsize)) ) {
						const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
						const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
						for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
							if ((sect->addr <= unslidInteriorAddress) && (unslidInteriorAddress < (sect->addr+sect->size))) {
								if ( segmentName != NULL )
									*segmentName = sect->segname;
								if ( sectionName != NULL )
									*sectionName = sect->sectname;
								if ( sectionOffset != NULL )
									*sectionOffset = unslidInteriorAddress - sect->addr;
								return true;
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return false;
}


void __attribute__((noreturn)) ImageLoaderMachO::throwSymbolNotFound(const LinkContext& context, const char* symbol, 
																	const char* referencedFrom, const char* fromVersMismatch,
																	const char* expectedIn)
{
	// record values for possible use by CrashReporter or Finder
	(*context.setErrorStrings)(dyld_error_kind_symbol_missing, referencedFrom, expectedIn, symbol);
	dyld::throwf("Symbol not found: %s\n  Referenced from: %s%s\n  Expected in: %s\n",
					symbol, referencedFrom, fromVersMismatch, expectedIn);
}

const mach_header* ImageLoaderMachO::machHeader() const
{
	return (mach_header*)fMachOData;
}

uintptr_t ImageLoaderMachO::getSlide() const
{
	return fSlide;
}

// hmm. maybe this should be up in ImageLoader??
const void* ImageLoaderMachO::getEnd() const
{
	uintptr_t lastAddress = 0;
	for(unsigned int i=0; i < fSegmentsCount; ++i) {
		uintptr_t segEnd = segActualEndAddress(i);
		if ( strcmp(segName(i), "__UNIXSTACK") != 0 ) {
			if ( segEnd > lastAddress )
				lastAddress = segEnd;
		}
	}
	return (const void*)lastAddress;
}


uintptr_t ImageLoaderMachO::bindLocation(const LinkContext& context, uintptr_t location, uintptr_t value, 
										const ImageLoader* targetImage, uint8_t type, const char* symbolName, 
										intptr_t addend, const char* msg)
{
	// log
	if ( context.verboseBind ) {
		if ( addend != 0 )
			dyld::log("dyld: %sbind: %s:0x%08lX = %s:%s, *0x%08lX = 0x%08lX + %ld\n",
						msg, this->getShortName(), (uintptr_t)location,
						((targetImage != NULL) ? targetImage->getShortName() : "<weak_import-missing>"),
						symbolName, (uintptr_t)location, value, addend);
		else
			dyld::log("dyld: %sbind: %s:0x%08lX = %s:%s, *0x%08lX = 0x%08lX\n",
						msg, this->getShortName(), (uintptr_t)location,
						((targetImage != NULL) ? targetImage->getShortName() : "<weak>import-missing>"),
						 symbolName, (uintptr_t)location, value);
	}
#if LOG_BINDINGS
//	dyld::logBindings("%s: %s\n", targetImage->getShortName(), symbolName);
#endif
	
	// do actual update
	uintptr_t* locationToFix = (uintptr_t*)location;
	uint32_t* loc32;
	uintptr_t newValue = value+addend;
	uint32_t value32;
	switch (type) {
		case BIND_TYPE_POINTER:
			// test first so we don't needless dirty pages
			if ( *locationToFix != newValue )
				*locationToFix = newValue;
			break;
		case BIND_TYPE_TEXT_ABSOLUTE32:
			loc32 = (uint32_t*)locationToFix;
			value32 = (uint32_t)newValue;
			if ( *loc32 != value32 )
				*loc32 = value32;
			break;
		case BIND_TYPE_TEXT_PCREL32:
			loc32 = (uint32_t*)locationToFix;
			value32 = (uint32_t)(newValue - (((uintptr_t)locationToFix) + 4));
			if ( *loc32 != value32 )
				*loc32 = value32;
			break;
		default:
			dyld::throwf("bad bind type %d", type);
	}
	
	// update statistics
	++fgTotalBindFixups;
	
	return newValue;
}





#if SUPPORT_OLD_CRT_INITIALIZATION
// first 16 bytes of "start" in crt1.o
#if __i386__
	static uint8_t sStandardEntryPointInstructions[16] = { 0x6a, 0x00, 0x89, 0xe5, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x10, 0x8b, 0x5d, 0x04, 0x89, 0x5c, 0x24 };
#endif
#endif

struct DATAdyld {
	void*			dyldLazyBinder;		// filled in at launch by dyld to point into dyld to &stub_binding_helper
	void*			dyldFuncLookup;		// filled in at launch by dyld to point into dyld to &_dyld_func_lookup
	// the following only exist in main executables built for 10.5 or later
	ProgramVars		vars;
};

// These are defined in dyldStartup.s
extern "C" void stub_binding_helper();


void ImageLoaderMachO::setupLazyPointerHandler(const LinkContext& context)
{
	const macho_header* mh = (macho_header*)fMachOData;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd;
	// There used to be some optimizations to skip this section scan, but we need to handle the 
	// __dyld section in libdyld.dylib, so everything needs to be scanned for now.
	// <rdar://problem/10910062> CrashTracer: 1,295 crashes in bash at bash: getenv
	if ( true ) {
		cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				if ( strcmp(seg->segname, "__DATA") == 0 ) {
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( strcmp(sect->sectname, "__dyld" ) == 0 ) {
							struct DATAdyld* dd = (struct DATAdyld*)(sect->addr + fSlide);
				#if !__arm64__
							if ( sect->size > offsetof(DATAdyld, dyldLazyBinder) ) {
								if ( dd->dyldLazyBinder != (void*)&stub_binding_helper )
									dd->dyldLazyBinder = (void*)&stub_binding_helper;
							}
				#endif // !__arm64__
							if ( sect->size > offsetof(DATAdyld, dyldFuncLookup) ) {
								if ( dd->dyldFuncLookup != (void*)&_dyld_func_lookup )
									dd->dyldFuncLookup = (void*)&_dyld_func_lookup;
							}
							if ( mh->filetype == MH_EXECUTE ) {
								// there are two ways to get the program variables
								if ( (sect->size > offsetof(DATAdyld, vars)) && (dd->vars.mh == mh) ) {
									// some really old binaries have space for vars, but it is zero filled
									// main executable has 10.5 style __dyld section that has program variable pointers
									context.setNewProgramVars(dd->vars);
								}
								else {
									// main executable is pre-10.5 and requires the symbols names to be looked up
									this->lookupProgramVars(context);
				#if SUPPORT_OLD_CRT_INITIALIZATION
									// If the first 16 bytes of the entry point's instructions do not 
									// match what crt1.o supplies, then the program has a custom entry point.
									// This means it might be doing something that needs to be executed before 
									// initializers are run. 
									if ( memcmp(this->getMain(), sStandardEntryPointInstructions, 16) != 0 ) {
										if ( context.verboseInit )
											dyld::log("dyld: program uses non-standard entry point so delaying running of initializers\n");
										context.setRunInitialzersOldWay();
									}
				#endif
								}
							}
							else if ( mh->filetype == MH_DYLIB ) {
								const char* installPath = this->getInstallPath();
								if ( (installPath != NULL) && (strncmp(installPath, "/usr/lib/", 9) == 0) ) {
									if ( sect->size > offsetof(DATAdyld, vars) ) {
										// use ProgramVars from libdyld.dylib but tweak mh field to correct value
										dd->vars.mh = context.mainExecutable->machHeader();
										context.setNewProgramVars(dd->vars);
									}
								}
							}
						}
						else if ( (strcmp(sect->sectname, "__program_vars" ) == 0) && (mh->filetype == MH_EXECUTE) ) {
							// this is a Mac OS X 10.6 or later main executable 
							struct ProgramVars* pv = (struct ProgramVars*)(sect->addr + fSlide);
							context.setNewProgramVars(*pv);
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}

									
void ImageLoaderMachO::lookupProgramVars(const LinkContext& context) const
{
	ProgramVars vars = context.programVars;
	const ImageLoader::Symbol* sym;
	
	// get mach header directly
	vars.mh = (macho_header*)fMachOData;
	
	// lookup _NXArgc
	sym = this->findExportedSymbol("_NXArgc", false, NULL);
	if ( sym != NULL )
		vars.NXArgcPtr = (int*)this->getExportedSymbolAddress(sym, context, this, false);
		
	// lookup _NXArgv
	sym = this->findExportedSymbol("_NXArgv", false, NULL);
	if ( sym != NULL )
		vars.NXArgvPtr = (const char***)this->getExportedSymbolAddress(sym, context, this, false);
		
	// lookup _environ
	sym = this->findExportedSymbol("_environ", false, NULL);
	if ( sym != NULL )
		vars.environPtr = (const char***)this->getExportedSymbolAddress(sym, context, this, false);
		
	// lookup __progname
	sym = this->findExportedSymbol("___progname", false, NULL);
	if ( sym != NULL )
		vars.__prognamePtr = (const char**)this->getExportedSymbolAddress(sym, context, this, false);
		
	context.setNewProgramVars(vars);
}


bool ImageLoaderMachO::usablePrebinding(const LinkContext& context) const
{
	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	if ( ((this->isPrebindable() && (this->getSlide() == 0)) || fInSharedCache)
		&& this->usesTwoLevelNameSpace()
		&& this->allDependentLibrariesAsWhenPreBound() ) {
		// allow environment variables to disable prebinding
		if ( context.bindFlat )
			return false;
		switch ( context.prebindUsage ) {
			case kUseAllPrebinding:
				return true;
			case kUseSplitSegPrebinding:
				return this->fIsSplitSeg;
			case kUseAllButAppPredbinding:
				return (this != context.mainExecutable);
			case kUseNoPrebinding:
				return false;
		}
	}
	return false;
}


void ImageLoaderMachO::doImageInit(const LinkContext& context)
{
	if ( fHasDashInit ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_ROUTINES_COMMAND:
					Initializer func = (Initializer)(((struct macho_routines_command*)cmd)->init_address + fSlide);
					// <rdar://problem/8543820&9228031> verify initializers are in image
					if ( ! this->containsAddress((void*)func) ) {
						dyld::throwf("initializer function %p not in mapped image for %s\n", func, this->getPath());
					}
					if ( context.verboseInit )
						dyld::log("dyld: calling -init function %p in %s\n", func, this->getPath());
					func(context.argc, context.argv, context.envp, context.apple, &context.programVars);
					break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}

void ImageLoaderMachO::doModInitFunctions(const LinkContext& context)
{
	if ( fHasInitializers ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
				const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
				for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					const uint8_t type = sect->flags & SECTION_TYPE;
					if ( type == S_MOD_INIT_FUNC_POINTERS ) {
						Initializer* inits = (Initializer*)(sect->addr + fSlide);
						const size_t count = sect->size / sizeof(uintptr_t);
						for (size_t i=0; i < count; ++i) {
							Initializer func = inits[i];
							// <rdar://problem/8543820&9228031> verify initializers are in image
							if ( ! this->containsAddress((void*)func) ) {
								dyld::throwf("initializer function %p not in mapped image for %s\n", func, this->getPath());
							}
							if ( context.verboseInit )
								dyld::log("dyld: calling initializer function %p in %s\n", func, this->getPath());
							func(context.argc, context.argv, context.envp, context.apple, &context.programVars);
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}






void ImageLoaderMachO::doGetDOFSections(const LinkContext& context, std::vector<ImageLoader::DOFInfo>& dofs)
{
	if ( fHasDOFSections ) {
		// walk load commands (mapped in at start of __TEXT segment)
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_SEGMENT_COMMAND:
					{
						const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
						const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
						const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
						for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
							if ( (sect->flags & SECTION_TYPE) == S_DTRACE_DOF ) {
								ImageLoader::DOFInfo info;
								info.dof			= (void*)(sect->addr + fSlide);
								info.imageHeader	= this->machHeader();
								info.imageShortName = this->getShortName();
								dofs.push_back(info);
							}
						}
					}
					break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}	


bool ImageLoaderMachO::doInitialization(const LinkContext& context)
{
	CRSetCrashLogMessage2(this->getPath());

	// mach-o has -init and static initializers
	doImageInit(context);
	doModInitFunctions(context);
	
	CRSetCrashLogMessage2(NULL);
	
	return (fHasDashInit || fHasInitializers);
}

bool ImageLoaderMachO::needsInitialization()
{
	return ( fHasDashInit || fHasInitializers );
}


bool ImageLoaderMachO::needsTermination()
{
	return fHasTerminators;
}


void ImageLoaderMachO::doTermination(const LinkContext& context)
{
	if ( fHasTerminators ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
				const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
				for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					const uint8_t type = sect->flags & SECTION_TYPE;
					if ( type == S_MOD_TERM_FUNC_POINTERS ) {
						Terminator* terms = (Terminator*)(sect->addr + fSlide);
						const size_t count = sect->size / sizeof(uintptr_t);
						for (size_t i=count; i > 0; --i) {
							Terminator func = terms[i-1];
							// <rdar://problem/8543820&9228031> verify terminators are in image
							if ( ! this->containsAddress((void*)func) ) {
								dyld::throwf("termination function %p not in mapped image for %s\n", func, this->getPath());
							}
							if ( context.verboseInit )
								dyld::log("dyld: calling termination function %p in %s\n", func, this->getPath());
							func();
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}


void ImageLoaderMachO::printStatistics(unsigned int imageCount, const InitializerTimingList& timingInfo)
{
	ImageLoader::printStatistics(imageCount, timingInfo);
	dyld::log("total symbol trie searches:    %d\n", fgSymbolTrieSearchs);
	dyld::log("total symbol table binary searches:    %d\n", fgSymbolTableBinarySearchs);
	dyld::log("total images defining weak symbols:  %u\n", fgImagesHasWeakDefinitions);
	dyld::log("total images using weak symbols:  %u\n", fgImagesRequiringCoalescing);
}


intptr_t ImageLoaderMachO::assignSegmentAddresses(const LinkContext& context)
{
	// preflight and calculate slide if needed
	const bool inPIE = (fgNextPIEDylibAddress != 0);
	intptr_t slide = 0;
	if ( this->segmentsCanSlide() && this->segmentsMustSlideTogether() ) {
		bool needsToSlide = false;
		bool imageHasPreferredLoadAddress = segHasPreferredLoadAddress(0);
		uintptr_t lowAddr = (unsigned long)(-1);
		uintptr_t highAddr = 0;
		for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
			const uintptr_t segLow = segPreferredLoadAddress(i);
			const uintptr_t segHigh = dyld_page_round(segLow + segSize(i));
			if ( segLow < highAddr ) {
				if ( dyld_page_size > 4096 )
					dyld::throwf("can't map segments into 16KB pages");
				else
					dyld::throwf("overlapping segments");
			}
			if ( segLow < lowAddr )
				lowAddr = segLow;
			if ( segHigh > highAddr )
				highAddr = segHigh;
				
			if ( needsToSlide || !imageHasPreferredLoadAddress || inPIE || !reserveAddressRange(segPreferredLoadAddress(i), segSize(i)) )
				needsToSlide = true;
		}
		if ( needsToSlide ) {
			// find a chunk of address space to hold all segments
			uintptr_t addr = reserveAnAddressRange(highAddr-lowAddr, context);
			slide = addr - lowAddr;
		}
	} 
	else if ( ! this->segmentsCanSlide() ) {
		for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
			if ( strcmp(segName(i), "__PAGEZERO") == 0 )
				continue;
			if ( !reserveAddressRange(segPreferredLoadAddress(i), segSize(i)) )
				dyld::throwf("can't map unslidable segment %s to 0x%lX with size 0x%lX", segName(i), segPreferredLoadAddress(i), segSize(i));
		}
	}
	else {
		throw "mach-o does not support independently sliding segments";
	}
	return slide;
}


uintptr_t ImageLoaderMachO::reserveAnAddressRange(size_t length, const ImageLoader::LinkContext& context)
{
	vm_address_t addr = 0;
	vm_size_t size = length;
	// in PIE programs, load initial dylibs after main executable so they don't have fixed addresses either
	if ( fgNextPIEDylibAddress != 0 ) {
		 // add small (0-3 pages) random padding between dylibs
		addr = fgNextPIEDylibAddress + (__stack_chk_guard/fgNextPIEDylibAddress & (sizeof(long)-1))*dyld_page_size;
		//dyld::log("padding 0x%08llX, guard=0x%08llX\n", (long long)(addr - fgNextPIEDylibAddress), (long long)(__stack_chk_guard));
		kern_return_t r = vm_alloc(&addr, size, VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_DYLIB));
		if ( r == KERN_SUCCESS ) {
			fgNextPIEDylibAddress = addr + size;
			return addr;
		}
		fgNextPIEDylibAddress = 0;
	}
	kern_return_t r = vm_alloc(&addr, size, VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_DYLIB));
	if ( r != KERN_SUCCESS ) 
		throw "out of address space";
	
	return addr;
}

bool ImageLoaderMachO::reserveAddressRange(uintptr_t start, size_t length)
{
	vm_address_t addr = start;
	vm_size_t size = length;
	kern_return_t r = vm_alloc(&addr, size, VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_DYLIB));
	if ( r != KERN_SUCCESS ) 
		return false;
	return true;
}



void ImageLoaderMachO::mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context)
{
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	if ( context.verboseMapping ) {
		if ( offsetInFat != 0 )
			dyld::log("dyld: Mapping %s (slice offset=%llu)\n", this->getPath(), (unsigned long long)offsetInFat);
		else
			dyld::log("dyld: Mapping %s\n", this->getPath());
	}
	// map in all segments
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		vm_offset_t fileOffset = segFileOffset(i) + offsetInFat;
		vm_size_t size = segFileSize(i);
		uintptr_t requestedLoadAddress = segPreferredLoadAddress(i) + slide;
		int protection = 0;
		if ( !segUnaccessible(i) ) {
			// If has text-relocs, don't set x-bit initially.
			// Instead set it later after text-relocs have been done.
			if ( segExecutable(i) && !(segHasRebaseFixUps(i) && (slide != 0)) )
				protection   |= PROT_EXEC;
			if ( segReadable(i) )
				protection   |= PROT_READ;
			if ( segWriteable(i) )
				protection   |= PROT_WRITE;
		}
	#if __i386__
		// initially map __IMPORT segments R/W so dyld can update them
		if ( segIsReadOnlyImport(i) )
			protection |= PROT_WRITE;
	#endif
		// wholly zero-fill segments have nothing to mmap() in
		if ( size > 0 ) {
			if ( (fileOffset+size) > fileLen ) {
				dyld::throwf("truncated mach-o error: segment %s extends to %llu which is past end of file %llu", 
								segName(i), (uint64_t)(fileOffset+size), fileLen);
			}
			void* loadAddress = xmmap((void*)requestedLoadAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, fileOffset);
			if ( loadAddress == ((void*)(-1)) ) {
				dyld::throwf("mmap() error %d at address=0x%08lX, size=0x%08lX segment=%s in Segment::map() mapping %s", 
					errno, requestedLoadAddress, (uintptr_t)size, segName(i), getPath());
			}
		}
		// update stats
		++ImageLoader::fgTotalSegmentsMapped;
		ImageLoader::fgTotalBytesMapped += size;
		if ( context.verboseMapping )
			dyld::log("%18s at 0x%08lX->0x%08lX with permissions %c%c%c\n", segName(i), requestedLoadAddress, requestedLoadAddress+size-1,
				(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
	}

	// update slide to reflect load location			
	this->setSlide(slide);
}

void ImageLoaderMachO::mapSegments(const void* memoryImage, uint64_t imageLen, const LinkContext& context)
{
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	if ( context.verboseMapping )
		dyld::log("dyld: Mapping memory %p\n", memoryImage);
	// map in all segments
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		vm_address_t loadAddress = segPreferredLoadAddress(i) + slide;
		vm_address_t srcAddr = (uintptr_t)memoryImage + segFileOffset(i);
		vm_size_t size = segFileSize(i);
		kern_return_t r = vm_copy(mach_task_self(), srcAddr, size, loadAddress);
		if ( r != KERN_SUCCESS ) 
			throw "can't map segment";
		if ( context.verboseMapping )
			dyld::log("%18s at 0x%08lX->0x%08lX\n", segName(i), (uintptr_t)loadAddress, (uintptr_t)loadAddress+size-1);
	}
	// update slide to reflect load location			
	this->setSlide(slide);
	// set R/W permissions on all segments at slide location
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		segProtect(i, context);		
	}
}


void ImageLoaderMachO::segProtect(unsigned int segIndex, const ImageLoader::LinkContext& context)
{
	vm_prot_t protection = 0;
	if ( !segUnaccessible(segIndex) ) {
		if ( segExecutable(segIndex) )
			protection   |= PROT_EXEC;
		if ( segReadable(segIndex) )
			protection   |= PROT_READ;
		if ( segWriteable(segIndex) )
			protection   |= PROT_WRITE;
	}
	vm_address_t addr = segActualLoadAddress(segIndex);
	vm_size_t size = segSize(segIndex);
	const bool setCurrentPermissions = false;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, protection);
	if ( r != KERN_SUCCESS ) {
        dyld::throwf("vm_protect(0x%08llX, 0x%08llX, false, 0x%02X) failed, result=%d for segment %s in %s",
            (long long)addr, (long long)size, protection, r, segName(segIndex), this->getPath());
    }
	if ( context.verboseMapping ) {
		dyld::log("%18s at 0x%08lX->0x%08lX altered permissions to %c%c%c\n", segName(segIndex), (uintptr_t)addr, (uintptr_t)addr+size-1,
			(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
	}
}	

void ImageLoaderMachO::segMakeWritable(unsigned int segIndex, const ImageLoader::LinkContext& context)
{
	vm_address_t addr = segActualLoadAddress(segIndex);
	vm_size_t size = segSize(segIndex);
	const bool setCurrentPermissions = false;
	vm_prot_t protection = VM_PROT_WRITE | VM_PROT_READ;
	if ( segExecutable(segIndex) && !segHasRebaseFixUps(segIndex) )
		protection |= VM_PROT_EXECUTE;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, protection);
	if ( r != KERN_SUCCESS ) {
        dyld::throwf("vm_protect(0x%08llX, 0x%08llX, false, 0x%02X) failed, result=%d for segment %s in %s",
            (long long)addr, (long long)size, protection, r, segName(segIndex), this->getPath());
    }
	if ( context.verboseMapping ) {
		dyld::log("%18s at 0x%08lX->0x%08lX altered permissions to %c%c%c\n", segName(segIndex), (uintptr_t)addr, (uintptr_t)addr+size-1,
			(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
	}
}


