/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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


#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <sys/param.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <mach-o/loader.h> 

#include "ImageLoaderMachOCompressed.h"
#include "mach-o/dyld_images.h"


// relocation_info.r_length field has value 3 for 64-bit executables and value 2 for 32-bit executables
#if __LP64__
	#define RELOC_SIZE 3
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define LC_ROUTINES_COMMAND		LC_ROUTINES_64
	struct macho_segment_command	: public segment_command_64  {};
	struct macho_section			: public section_64  {};	
	struct macho_routines_command	: public routines_command_64  {};	
#else
	#define RELOC_SIZE 2
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define LC_ROUTINES_COMMAND		LC_ROUTINES
	struct macho_segment_command	: public segment_command {};
	struct macho_section			: public section  {};	
	struct macho_routines_command	: public routines_command  {};	
#endif

	
static uintptr_t read_uleb128(const uint8_t*& p, const uint8_t* end)
{
	uint64_t result = 0;
	int		 bit = 0;
	do {
		if (p == end)
			dyld::throwf("malformed uleb128");

		uint64_t slice = *p & 0x7f;

		if (bit >= 64 || slice << bit >> bit != slice)
			dyld::throwf("uleb128 too big");
		else {
			result |= (slice << bit);
			bit += 7;
		}
	} 
	while (*p++ & 0x80);
	return result;
}


static intptr_t read_sleb128(const uint8_t*& p, const uint8_t* end)
{
	int64_t result = 0;
	int bit = 0;
	uint8_t byte;
	do {
		if (p == end)
			throw "malformed sleb128";
		byte = *p++;
		result |= ((byte & 0x7f) << bit);
		bit += 7;
	} while (byte & 0x80);
	// sign extend negative numbers
	if ( (byte & 0x40) != 0 )
		result |= (-1LL) << bit;
	return result;
}


// create image for main executable
ImageLoaderMachOCompressed* ImageLoaderMachOCompressed::instantiateMainExecutable(const macho_header* mh, uintptr_t slide, const char* path, 
																		unsigned int segCount, unsigned int libCount, const LinkContext& context)
{
	ImageLoaderMachOCompressed* image = ImageLoaderMachOCompressed::instantiateStart(mh, path, segCount, libCount);

	// set slide for PIE programs
	image->setSlide(slide);

	// for PIE record end of program, to know where to start loading dylibs
	if ( (mh->flags & MH_PIE) && !context.noPIE )
		fgNextPIEDylibAddress = (uintptr_t)image->getEnd();
	
	image->setNeverUnload();
	image->instantiateFinish(context);
	
	if ( context.verboseMapping ) {
		dyld::log("dyld: Main executable mapped %s\n", path);
		for(unsigned int i=0, e=image->segmentCount(); i < e; ++i) {
			const char* name = image->segName(i);
			if ( (strcmp(name, "__PAGEZERO") == 0) || (strcmp(name, "__UNIXSTACK") == 0)  )
				dyld::log("%18s at 0x%08lX->0x%08lX\n", name, image->segPreferredLoadAddress(i), image->segPreferredLoadAddress(i)+image->segSize(i));
			else
				dyld::log("%18s at 0x%08lX->0x%08lX\n", name, image->segActualLoadAddress(i), image->segActualEndAddress(i));
		}
	}

	return image;
}

// create image by mapping in a mach-o file
ImageLoaderMachOCompressed* ImageLoaderMachOCompressed::instantiateFromFile(const char* path, int fd, const uint8_t* fileData, 
															uint64_t offsetInFat, uint64_t lenInFat, const struct stat& info, 
															unsigned int segCount, unsigned int libCount, const LinkContext& context)
{
	ImageLoaderMachOCompressed* image = ImageLoaderMachOCompressed::instantiateStart((macho_header*)fileData, path, segCount, libCount);

	try {
		// record info about file  
		image->setFileInfo(info.st_dev, info.st_ino, info.st_mtime);

		// mmap segments
		image->mapSegments(fd, offsetInFat, lenInFat, info.st_size, context);

		// finish construction
		image->instantiateFinish(context);
		
		// if path happens to be same as in LC_DYLIB_ID load command use that, otherwise malloc a copy of the path
		const char* installName = image->getInstallPath();
		if ( (installName != NULL) && (strcmp(installName, path) == 0) && (path[0] == '/') )
			image->setPathUnowned(installName);
#if __MAC_OS_X_VERSION_MIN_REQUIRED
		// <rdar://problem/6563887> app crashes when libSystem cannot be found
		else if ( (installName != NULL) && (strcmp(path, "/usr/lib/libgcc_s.1.dylib") == 0) && (strcmp(installName, "/usr/lib/libSystem.B.dylib") == 0) )
			image->setPathUnowned("/usr/lib/libSystem.B.dylib");
#endif
		else if ( path[0] != '/' ) {
			// rdar://problem/5135363 turn relative paths into absolute paths so gdb, Symbolication can later find them
			char realPath[MAXPATHLEN];
			if ( realpath(path, realPath) != NULL )
				image->setPath(realPath);
			else
				image->setPath(path);
		}
		else 
			image->setPath(path);

		// pre-fetch content of __DATA and __LINKEDIT segment for faster launches
		// don't do this on prebound images or if prefetching is disabled
        if ( !context.preFetchDisabled && !image->isPrebindable()) {
			image->preFetchDATA(fd, offsetInFat, context);
			image->markSequentialLINKEDIT(context);
		}
	}
	catch (...) {
		// ImageLoader::setMapped() can throw an exception to block loading of image
		// <rdar://problem/6169686> Leaked fSegmentsArray and image segments during failed dlopen_preflight
		delete image;
		throw;
	}
	
	return image;
}

// create image by using cached mach-o file
ImageLoaderMachOCompressed* ImageLoaderMachOCompressed::instantiateFromCache(const macho_header* mh, const char* path, const struct stat& info,
																unsigned int segCount, unsigned int libCount, const LinkContext& context)
{
	ImageLoaderMachOCompressed* image = ImageLoaderMachOCompressed::instantiateStart(mh, path, segCount, libCount);
	try {
		// record info about file  
		image->setFileInfo(info.st_dev, info.st_ino, info.st_mtime);

		// remember this is from shared cache and cannot be unloaded
		image->fInSharedCache = true;
		image->setNeverUnload();

		// segments already mapped in cache
		if ( context.verboseMapping ) {
			dyld::log("dyld: Using shared cached for %s\n", path);
			for(unsigned int i=0; i < image->fSegmentsCount; ++i) {
				dyld::log("%18s at 0x%08lX->0x%08lX\n", image->segName(i), image->segActualLoadAddress(i), image->segActualEndAddress(i));
			}
		}

		image->instantiateFinish(context);
	}
	catch (...) {
		// ImageLoader::setMapped() can throw an exception to block loading of image
		// <rdar://problem/6169686> Leaked fSegmentsArray and image segments during failed dlopen_preflight
		delete image;
		throw;
	}
	
	return image;
}

// create image by copying an in-memory mach-o file
ImageLoaderMachOCompressed* ImageLoaderMachOCompressed::instantiateFromMemory(const char* moduleName, const macho_header* mh, uint64_t len, 
															unsigned int segCount, unsigned int libCount, const LinkContext& context)
{
	ImageLoaderMachOCompressed* image = ImageLoaderMachOCompressed::instantiateStart(mh, moduleName, segCount, libCount);
	try {
		// map segments 
		if ( mh->filetype == MH_EXECUTE ) 
			throw "can't load another MH_EXECUTE";
		
		// vmcopy segments
		image->mapSegments((const void*)mh, len, context);
		
		// for compatibility, never unload dylibs loaded from memory
		image->setNeverUnload();

		// bundle loads need path copied
		if ( moduleName != NULL ) 
			image->setPath(moduleName);

		image->instantiateFinish(context);
	}
	catch (...) {
		// ImageLoader::setMapped() can throw an exception to block loading of image
		// <rdar://problem/6169686> Leaked fSegmentsArray and image segments during failed dlopen_preflight
		delete image;
		throw;
	}
	
	return image;
}


ImageLoaderMachOCompressed::ImageLoaderMachOCompressed(const macho_header* mh, const char* path, unsigned int segCount, 
																		uint32_t segOffsets[], unsigned int libCount)
 : ImageLoaderMachO(mh, path, segCount, segOffsets, libCount), fDyldInfo(NULL)
{
}

ImageLoaderMachOCompressed::~ImageLoaderMachOCompressed()
{
	// don't do clean up in ~ImageLoaderMachO() because virtual call to segmentCommandOffsets() won't work
	destroy();
}



// construct ImageLoaderMachOCompressed using "placement new" with SegmentMachO objects array at end
ImageLoaderMachOCompressed* ImageLoaderMachOCompressed::instantiateStart(const macho_header* mh, const char* path, 
																			unsigned int segCount, unsigned int libCount)
{
	size_t size = sizeof(ImageLoaderMachOCompressed) + segCount * sizeof(uint32_t) + libCount * sizeof(ImageLoader*);
	ImageLoaderMachOCompressed* allocatedSpace = static_cast<ImageLoaderMachOCompressed*>(malloc(size));
	if ( allocatedSpace == NULL )
		throw "malloc failed";
	uint32_t* segOffsets = ((uint32_t*)(((uint8_t*)allocatedSpace) + sizeof(ImageLoaderMachOCompressed)));
	bzero(&segOffsets[segCount], libCount*sizeof(void*));	// zero out lib array
	return new (allocatedSpace) ImageLoaderMachOCompressed(mh, path, segCount, segOffsets, libCount);
}


// common code to finish initializing object
void ImageLoaderMachOCompressed::instantiateFinish(const LinkContext& context)
{
	// now that segments are mapped in, get real fMachOData, fLinkEditBase, and fSlide
	this->parseLoadCmds();
		
	// notify state change
	this->setMapped(context);
}

uint32_t* ImageLoaderMachOCompressed::segmentCommandOffsets() const
{
	return ((uint32_t*)(((uint8_t*)this) + sizeof(ImageLoaderMachOCompressed)));
}


ImageLoader* ImageLoaderMachOCompressed::libImage(unsigned int libIndex) const
{
	const uintptr_t* images = ((uintptr_t*)(((uint8_t*)this) + sizeof(ImageLoaderMachOCompressed) + fSegmentsCount*sizeof(uint32_t)));
	// mask off low bit
	return (ImageLoader*)(images[libIndex] & (-2));
}

bool ImageLoaderMachOCompressed::libReExported(unsigned int libIndex) const
{
	const uintptr_t* images = ((uintptr_t*)(((uint8_t*)this) + sizeof(ImageLoaderMachOCompressed) + fSegmentsCount*sizeof(uint32_t)));
	// re-export flag is low bit
	return ((images[libIndex] & 1) != 0);
}	


void ImageLoaderMachOCompressed::setLibImage(unsigned int libIndex, ImageLoader* image, bool reExported)
{
	uintptr_t* images = ((uintptr_t*)(((uint8_t*)this) + sizeof(ImageLoaderMachOCompressed) + fSegmentsCount*sizeof(uint32_t)));
	uintptr_t value = (uintptr_t)image;
	if ( reExported ) 
		value |= 1;
	images[libIndex] = value;
}


void ImageLoaderMachOCompressed::markFreeLINKEDIT(const LinkContext& context)
{
	// mark that we are done with rebase and bind info 
	markLINKEDIT(context, MADV_FREE);
}

void ImageLoaderMachOCompressed::markSequentialLINKEDIT(const LinkContext& context)
{
	// mark the rebase and bind info and using sequential access
	markLINKEDIT(context, MADV_SEQUENTIAL);
}

void ImageLoaderMachOCompressed::markLINKEDIT(const LinkContext& context, int advise)
{
	// if not loaded at preferred address, mark rebase info 
	uintptr_t start = 0;
	if ( (fSlide != 0) && (fDyldInfo->rebase_size != 0) ) 
		start = (uintptr_t)fLinkEditBase + fDyldInfo->rebase_off;
	else if ( fDyldInfo->bind_off != 0 )
		start = (uintptr_t)fLinkEditBase + fDyldInfo->bind_off;
	else
		return; // no binding info to prefetch
		
	// end is at end of bind info
	uintptr_t end = 0;
	if ( fDyldInfo->bind_off != 0 )
		end = (uintptr_t)fLinkEditBase + fDyldInfo->bind_off + fDyldInfo->bind_size;
	else if ( fDyldInfo->rebase_off != 0 )
		end = (uintptr_t)fLinkEditBase + fDyldInfo->rebase_off + fDyldInfo->rebase_size;
	else
		return;
		
			
	// round to whole pages
	start = start & (-4096);
	end = (end + 4095) & (-4096);

	// do nothing if only one page of rebase/bind info
	if ( (end-start) <= 4096 )
		return;
	
	// tell kernel about our access to these pages
	madvise((void*)start, end-start, advise);
	if ( context.verboseMapping ) {
		const char* adstr = "sequential";
		if ( advise == MADV_FREE )
			adstr = "free";
		dyld::log("%18s %s 0x%0lX -> 0x%0lX\n", "__LINKEDIT", adstr, start, end-1);
	}
}



void ImageLoaderMachOCompressed::rebaseAt(const LinkContext& context, uintptr_t addr, uintptr_t slide, uint8_t type)
{
	//dyld::log("0x%08lX type=%d\n", addr, type);
	uintptr_t* locationToFix = (uintptr_t*)addr;
	switch (type) {
		case REBASE_TYPE_POINTER:
			*locationToFix += slide;
			break;
		case REBASE_TYPE_TEXT_ABSOLUTE32:
			*locationToFix += slide;
			break;
		default:
			dyld::throwf("bad rebase type %d", type);
	}
}

void ImageLoaderMachOCompressed::throwBadRebaseAddress(uintptr_t address, uintptr_t segmentEndAddress, int segmentIndex, 
										const uint8_t* startOpcodes, const uint8_t* endOpcodes, const uint8_t* pos)
{
	dyld::throwf("malformed rebase opcodes (%ld/%ld): address 0x%08lX is beyond end of segment %s (0x%08lX -> 0x%08lX)",
		(intptr_t)(pos-startOpcodes), (intptr_t)(endOpcodes-startOpcodes), address, segName(segmentIndex), 
		segActualLoadAddress(segmentIndex), segmentEndAddress); 
}

void ImageLoaderMachOCompressed::rebase(const LinkContext& context)
{
	const uintptr_t slide = this->fSlide;
	const uint8_t* const start = fLinkEditBase + fDyldInfo->rebase_off;
	const uint8_t* const end = &start[fDyldInfo->rebase_size];
	const uint8_t* p = start;

	try {
		uint8_t type = 0;
		int segmentIndex = 0;
		uintptr_t address = segActualLoadAddress(0);
		uintptr_t segmentEndAddress = segActualEndAddress(0);
		uint32_t count;
		uint32_t skip;
		bool done = false;
		while ( !done && (p < end) ) {
			uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
			uint8_t opcode = *p & REBASE_OPCODE_MASK;
			++p;
			switch (opcode) {
				case REBASE_OPCODE_DONE:
					done = true;
					break;
				case REBASE_OPCODE_SET_TYPE_IMM:
					type = immediate;
					break;
				case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
					segmentIndex = immediate;
					if ( segmentIndex > fSegmentsCount )
						dyld::throwf("REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
								segmentIndex, fSegmentsCount);
					address = segActualLoadAddress(segmentIndex) + read_uleb128(p, end);
					segmentEndAddress = segActualEndAddress(segmentIndex);
					break;
				case REBASE_OPCODE_ADD_ADDR_ULEB:
					address += read_uleb128(p, end);
					break;
				case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
					address += immediate*sizeof(uintptr_t);
					break;
				case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
					for (int i=0; i < immediate; ++i) {
						if ( address >= segmentEndAddress ) 
							throwBadRebaseAddress(address, segmentEndAddress, segmentIndex, start, end, p);
						rebaseAt(context, address, slide, type);
						address += sizeof(uintptr_t);
					}
					fgTotalRebaseFixups += immediate;
					break;
				case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
					count = read_uleb128(p, end);
					for (uint32_t i=0; i < count; ++i) {
						if ( address >= segmentEndAddress ) 
							throwBadRebaseAddress(address, segmentEndAddress, segmentIndex, start, end, p);
						rebaseAt(context, address, slide, type);
						address += sizeof(uintptr_t);
					}
					fgTotalRebaseFixups += count;
					break;
				case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
					if ( address >= segmentEndAddress ) 
						throwBadRebaseAddress(address, segmentEndAddress, segmentIndex, start, end, p);
					rebaseAt(context, address, slide, type);
					address += read_uleb128(p, end) + sizeof(uintptr_t);
					++fgTotalRebaseFixups;
					break;
				case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
					count = read_uleb128(p, end);
					skip = read_uleb128(p, end);
					for (uint32_t i=0; i < count; ++i) {
						if ( address >= segmentEndAddress ) 
							throwBadRebaseAddress(address, segmentEndAddress, segmentIndex, start, end, p);
						rebaseAt(context, address, slide, type);
						address += skip + sizeof(uintptr_t);
					}
					fgTotalRebaseFixups += count;
					break;
				default:
					dyld::throwf("bad rebase opcode %d", *p);
			}
		}
	}
	catch (const char* msg) {
		const char* newMsg = dyld::mkstringf("%s in %s", msg, this->getPath());
		free((void*)msg);
		throw newMsg;
	}
}




const ImageLoader::Symbol* ImageLoaderMachOCompressed::findExportedSymbol(const char* symbol, const ImageLoader** foundIn) const
{
	//dyld::log("findExportedSymbolCompressed(%s) in %s\n", symbol, this->getShortName());
	if ( fDyldInfo->export_size == 0 )
		return NULL;
	++ImageLoaderMachO::fgSymbolTrieSearchs;
	const uint8_t* start = &fLinkEditBase[fDyldInfo->export_off];
	const uint8_t* end = &start[fDyldInfo->export_size];
	const uint8_t* p = start;
	const char* s = symbol;
	do {
		const uint8_t terminalSize = *p++;
		const uint8_t* children = p + terminalSize;
		if ( (*s == '\0') && (terminalSize != 0) ) {
			// found match, return pointer to terminal part of node
			//dyld::log("findExportedSymbol(%s) in %s found match, returning %p\n", symbol, this->getShortName(), p);
			if ( foundIn != NULL )
				*foundIn = (ImageLoader*)this;		
			return (Symbol*)p;
		}
		const uint8_t childrenCount = *children++;
		const uint8_t* e = children;
		const uint8_t* newNode = NULL;
		for (uint8_t i=0; i < childrenCount; ++i) {
			const char* ss = s;
			bool wrongEdge = false;
			//dyld::log("findExportedSymbol() looking at edge %s for match to %s\n", e, s);
			// scan whole edge to get to next edge
			// if edge is longer than target symbol name, don't read past end of symbol name
			while ( *e != '\0' ) {
				if ( !wrongEdge ) {
					if ( *e != *ss++ )
						wrongEdge = true;
				}
				++e;
			}
			if ( wrongEdge ) {
				// advance to next child
				++e;
				read_uleb128(e, end);
			}
			else {
				// the symbol so far matches this edge (child)
				// so advance to the child's node
				++e;
				uint32_t nodeOffset = read_uleb128(e, end);
				newNode = &start[nodeOffset];
				s = ss;
				//dyld::log("findExportedSymbol() found matching edge advancing to node 0x%x\n", nodeOffset);
				break;
			}
		}
		if ( newNode != NULL )
			p = newNode;
		else {
			//dyld::log("findExportedSymbol(%s) in %s failed\n", symbol, this->getShortName());
			return NULL;
		}
	} while ( true );
}


bool ImageLoaderMachOCompressed::containsSymbol(const void* addr) const
{
	const uint8_t* start = &fLinkEditBase[fDyldInfo->export_off];
	const uint8_t* end = &start[fDyldInfo->export_size];
	return ( (start <= addr) && (addr < end) );
}


uintptr_t ImageLoaderMachOCompressed::exportedSymbolAddress(const Symbol* symbol) const
{
	const uint8_t* exportNode = (uint8_t*)symbol;
	const uint8_t* exportTrieStart = fLinkEditBase + fDyldInfo->export_off;
	const uint8_t* exportTrieEnd = exportTrieStart + fDyldInfo->export_size;
	if ( (exportNode < exportTrieStart) || (exportNode > exportTrieEnd) )
		throw "symbol is not in trie";
	uint32_t flags = read_uleb128(exportNode, exportTrieEnd);
	if ( (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_REGULAR )
		return read_uleb128(exportNode, exportTrieEnd) + (uintptr_t)fMachOData;
	else
		throw "unsupported exported symbol kind";
}

bool ImageLoaderMachOCompressed::exportedSymbolIsWeakDefintion(const Symbol* symbol) const
{
	const uint8_t* exportNode = (uint8_t*)symbol;
	const uint8_t* exportTrieStart = fLinkEditBase + fDyldInfo->export_off;
	const uint8_t* exportTrieEnd = exportTrieStart + fDyldInfo->export_size;
	if ( (exportNode < exportTrieStart) || (exportNode > exportTrieEnd) )
		throw "symbol is not in trie";
	uint32_t flags = read_uleb128(exportNode, exportTrieEnd);
	return ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION );
}


const char* ImageLoaderMachOCompressed::exportedSymbolName(const Symbol* symbol) const
{
	throw "NSNameOfSymbol() not supported with compressed LINKEDIT";
}

unsigned int ImageLoaderMachOCompressed::exportedSymbolCount() const
{
	throw "NSSymbolDefinitionCountInObjectFileImage() not supported with compressed LINKEDIT";
}

const ImageLoader::Symbol* ImageLoaderMachOCompressed::exportedSymbolIndexed(unsigned int index) const
{
	throw "NSSymbolDefinitionNameInObjectFileImage() not supported with compressed LINKEDIT";
}

unsigned int ImageLoaderMachOCompressed::importedSymbolCount() const
{
	throw "NSSymbolReferenceCountInObjectFileImage() not supported with compressed LINKEDIT";
}

const ImageLoader::Symbol* ImageLoaderMachOCompressed::importedSymbolIndexed(unsigned int index) const
{
	throw "NSSymbolReferenceCountInObjectFileImage() not supported with compressed LINKEDIT";
}

const char* ImageLoaderMachOCompressed::importedSymbolName(const Symbol* symbol) const
{
	throw "NSSymbolReferenceNameInObjectFileImage() not supported with compressed LINKEDIT";
}



uintptr_t ImageLoaderMachOCompressed::resolveFlat(const LinkContext& context, const char* symbolName, bool weak_import, const ImageLoader** foundIn)
{
	const Symbol* sym;
	if ( context.flatExportFinder(symbolName, &sym, foundIn) ) {
		if ( (*foundIn != this) && !(*foundIn)->neverUnload() )
				this->addDynamicReference(*foundIn);
		return (*foundIn)->getExportedSymbolAddress(sym, context, this);
	}
	// if a bundle is loaded privately the above will not find its exports
	if ( this->isBundle() && this->hasHiddenExports() ) {
		// look in self for needed symbol
		sym = this->ImageLoaderMachO::findExportedSymbol(symbolName, false, foundIn);
		if ( sym != NULL )
			return (*foundIn)->getExportedSymbolAddress(sym, context, this);
	}
	if ( weak_import ) {
		// definition can't be found anywhere, ok because it is weak, just return 0
		return 0;
	}
	throwSymbolNotFound(symbolName, this->getPath(), "flat namespace");
}


uintptr_t ImageLoaderMachOCompressed::resolveTwolevel(const LinkContext& context, const ImageLoader* targetImage, bool weak_import, 
												const char* symbolName, const ImageLoader** foundIn)
{
	// two level lookup
	const Symbol* sym = targetImage->findExportedSymbol(symbolName, true, foundIn);
	if ( sym != NULL ) {
		return (*foundIn)->getExportedSymbolAddress(sym, context, this);
	}
	
	if ( weak_import ) {
		// definition can't be found anywhere, ok because it is weak, just return 0
		return 0;
	}

	// nowhere to be found
	throwSymbolNotFound(symbolName, this->getPath(), targetImage->getPath());
}


uintptr_t ImageLoaderMachOCompressed::resolve(const LinkContext& context, const char* symbolName, 
													uint8_t symboFlags, int libraryOrdinal, const ImageLoader** targetImage,
													LastLookup* last)
{
	*targetImage = NULL;
	
	// only clients that benefit from caching last lookup pass in a LastLookup struct
	if ( last != NULL ) {
		if ( (last->ordinal == libraryOrdinal)
			&& (last->flags == symboFlags)
			&& (last->name == symbolName) ) {
				*targetImage = last->foundIn;
				return last->result;
			}
	}
	
	bool weak_import = (symboFlags & BIND_SYMBOL_FLAGS_WEAK_IMPORT);
	uintptr_t symbolAddress;
	if ( context.bindFlat || (libraryOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP) ) {
		symbolAddress = this->resolveFlat(context, symbolName, weak_import, targetImage);
	}
	else {
		if ( libraryOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
			*targetImage = context.mainExecutable;
		}
		else if ( libraryOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
			*targetImage = this;
		}
		else if ( libraryOrdinal <= 0 ) {
			dyld::throwf("bad mach-o binary, unknown special library ordinal (%u) too big for symbol %s in %s",
				libraryOrdinal, symbolName, this->getPath());
		}
		else if ( (unsigned)libraryOrdinal <= libraryCount() ) {
			*targetImage = libImage(libraryOrdinal-1);
		}
		else {
			dyld::throwf("bad mach-o binary, library ordinal (%u) too big (max %u) for symbol %s in %s",
				libraryOrdinal, libraryCount(), symbolName, this->getPath());
		}
		if ( *targetImage == NULL ) {
			if ( weak_import ) {
				// if target library not loaded and reference is weak or library is weak return 0
				symbolAddress = 0;
			}
			else {
				dyld::throwf("can't resolve symbol %s in %s because dependent dylib #%d could not be loaded",
					symbolName, this->getPath(), libraryOrdinal);
			}
		}
		else {
			symbolAddress = resolveTwolevel(context, *targetImage, weak_import, symbolName, targetImage);
		}
	}
	
	// save off lookup results if client wants 
	if ( last != NULL ) {
		last->ordinal	= libraryOrdinal;
		last->flags		= symboFlags;
		last->name		= symbolName;
		last->foundIn	= *targetImage;
		last->result	= symbolAddress;
	}
	
	return symbolAddress;
}

uintptr_t ImageLoaderMachOCompressed::bindAt(const LinkContext& context, uintptr_t addr, uint8_t type, const char* symbolName, 
								uint8_t symboFlags, intptr_t addend, int libraryOrdinal, const char* msg, LastLookup* last)
{
	const ImageLoader*	targetImage;
	uintptr_t			symbolAddress;
	
	// resolve symbol
	symbolAddress = this->resolve(context, symbolName, symboFlags, libraryOrdinal, &targetImage, last);

	// do actual update
	return this->bindLocation(context, addr, symbolAddress, targetImage, type, symbolName, addend, msg);
}

void ImageLoaderMachOCompressed::throwBadBindingAddress(uintptr_t address, uintptr_t segmentEndAddress, int segmentIndex, 
										const uint8_t* startOpcodes, const uint8_t* endOpcodes, const uint8_t* pos)
{
	dyld::throwf("malformed binding opcodes (%ld/%ld): address 0x%08lX is beyond end of segment %s (0x%08lX -> 0x%08lX)",
		(intptr_t)(pos-startOpcodes), (intptr_t)(endOpcodes-startOpcodes), address, segName(segmentIndex), 
		segActualLoadAddress(segmentIndex), segmentEndAddress); 
}


void ImageLoaderMachOCompressed::doBind(const LinkContext& context, bool forceLazysBound)
{
	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	// note: flat-namespace binaries need to have imports rebound (even if correctly prebound)
	if ( this->usablePrebinding(context) ) {
		// don't need to bind
	}
	else {
		// run through all binding opcodes
		eachBind(context, &ImageLoaderMachOCompressed::bindAt);
			
		// if this image is in the shared cache, but depends on someting no longer in the shared cache,
		// there is no way to reset the lazy pointers, so force bind them now
		if ( forceLazysBound || fInSharedCache )
			this->doBindJustLazies(context);
	}
	
	// set up dyld entry points in image
	// do last so flat main executables will have __dyld or __program_vars set up
	this->setupLazyPointerHandler(context);
	
	// tell kernel we are done with chunks of LINKEDIT
	if ( !context.preFetchDisabled ) 
		this->markFreeLINKEDIT(context);
}


void ImageLoaderMachOCompressed::doBindJustLazies(const LinkContext& context)
{
	eachLazyBind(context, &ImageLoaderMachOCompressed::bindAt);
}

void ImageLoaderMachOCompressed::eachBind(const LinkContext& context, bind_handler handler)
{
	try {
		uint8_t type = 0;
		int segmentIndex = 0;
		uintptr_t address = segActualLoadAddress(0);
		uintptr_t segmentEndAddress = segActualEndAddress(0);
		const char* symbolName = NULL;
		uint8_t symboFlags = 0;
		int libraryOrdinal = 0;
		intptr_t addend = 0;
		uint32_t count;
		uint32_t skip;
		LastLookup last = { 0, 0, NULL, 0, NULL };
		const uint8_t* const start = fLinkEditBase + fDyldInfo->bind_off;
		const uint8_t* const end = &start[fDyldInfo->bind_size];
		const uint8_t* p = start;
		bool done = false;
		while ( !done && (p < end) ) {
			uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
			uint8_t opcode = *p & BIND_OPCODE_MASK;
			++p;
			switch (opcode) {
				case BIND_OPCODE_DONE:
					done = true;
					break;
				case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
					libraryOrdinal = immediate;
					break;
				case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
					libraryOrdinal = read_uleb128(p, end);
					break;
				case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
					// the special ordinals are negative numbers
					if ( immediate == 0 )
						libraryOrdinal = 0;
					else {
						int8_t signExtended = BIND_OPCODE_MASK | immediate;
						libraryOrdinal = signExtended;
					}
					break;
				case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
					symbolName = (char*)p;
					symboFlags = immediate;
					while (*p != '\0')
						++p;
					++p;
					break;
				case BIND_OPCODE_SET_TYPE_IMM:
					type = immediate;
					break;
				case BIND_OPCODE_SET_ADDEND_SLEB:
					addend = read_sleb128(p, end);
					break;
				case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
					segmentIndex = immediate;
					if ( segmentIndex > fSegmentsCount )
						dyld::throwf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
								segmentIndex, fSegmentsCount);
					address = segActualLoadAddress(segmentIndex) + read_uleb128(p, end);
					segmentEndAddress = segActualEndAddress(segmentIndex);
					break;
				case BIND_OPCODE_ADD_ADDR_ULEB:
					address += read_uleb128(p, end);
					break;
				case BIND_OPCODE_DO_BIND:
					if ( address >= segmentEndAddress ) 
						throwBadBindingAddress(address, segmentEndAddress, segmentIndex, start, end, p);
					(this->*handler)(context, address, type, symbolName, symboFlags, addend, libraryOrdinal, "", &last);
					address += sizeof(intptr_t);
					break;
				case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
					if ( address >= segmentEndAddress ) 
						throwBadBindingAddress(address, segmentEndAddress, segmentIndex, start, end, p);
					(this->*handler)(context, address, type, symbolName, symboFlags, addend, libraryOrdinal, "", &last);
					address += read_uleb128(p, end) + sizeof(intptr_t);
					break;
				case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
					if ( address >= segmentEndAddress ) 
						throwBadBindingAddress(address, segmentEndAddress, segmentIndex, start, end, p);
					(this->*handler)(context, address, type, symbolName, symboFlags, addend, libraryOrdinal, "", &last);
					address += immediate*sizeof(intptr_t) + sizeof(intptr_t);
					break;
				case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
					count = read_uleb128(p, end);
					skip = read_uleb128(p, end);
					for (uint32_t i=0; i < count; ++i) {
						if ( address >= segmentEndAddress ) 
							throwBadBindingAddress(address, segmentEndAddress, segmentIndex, start, end, p);
						(this->*handler)(context, address, type, symbolName, symboFlags, addend, libraryOrdinal, "", &last);
						address += skip + sizeof(intptr_t);
					}
					break;
				default:
					dyld::throwf("bad bind opcode %d in bind info", *p);
			}
		}
	}
	catch (const char* msg) {
		const char* newMsg = dyld::mkstringf("%s in %s", msg, this->getPath());
		free((void*)msg);
		throw newMsg;
	}
}

void ImageLoaderMachOCompressed::eachLazyBind(const LinkContext& context, bind_handler handler)
{
	try {
		uint8_t type = BIND_TYPE_POINTER;
		int segmentIndex = 0;
		uintptr_t address = segActualLoadAddress(0);
		uintptr_t segmentEndAddress = segActualEndAddress(0);
		const char* symbolName = NULL;
		uint8_t symboFlags = 0;
		int libraryOrdinal = 0;
		intptr_t addend = 0;
		const uint8_t* const start = fLinkEditBase + fDyldInfo->lazy_bind_off;
		const uint8_t* const end = &start[fDyldInfo->lazy_bind_size];
		const uint8_t* p = start;
		bool done = false;
		while ( !done && (p < end) ) {
			uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
			uint8_t opcode = *p & BIND_OPCODE_MASK;
			++p;
			switch (opcode) {
				case BIND_OPCODE_DONE:
					// there is BIND_OPCODE_DONE at end of each lazy bind, don't stop until end of whole sequence
					break;
				case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
					libraryOrdinal = immediate;
					break;
				case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
					libraryOrdinal = read_uleb128(p, end);
					break;
				case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
					// the special ordinals are negative numbers
					if ( immediate == 0 )
						libraryOrdinal = 0;
					else {
						int8_t signExtended = BIND_OPCODE_MASK | immediate;
						libraryOrdinal = signExtended;
					}
					break;
				case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
					symbolName = (char*)p;
					symboFlags = immediate;
					while (*p != '\0')
						++p;
					++p;
					break;
				case BIND_OPCODE_SET_TYPE_IMM:
					type = immediate;
					break;
				case BIND_OPCODE_SET_ADDEND_SLEB:
					addend = read_sleb128(p, end);
					break;
				case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
					segmentIndex = immediate;
					if ( segmentIndex > fSegmentsCount )
						dyld::throwf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
								segmentIndex, fSegmentsCount);
					address = segActualLoadAddress(segmentIndex) + read_uleb128(p, end);
					segmentEndAddress = segActualEndAddress(segmentIndex);
					break;
				case BIND_OPCODE_ADD_ADDR_ULEB:
					address += read_uleb128(p, end);
					break;
				case BIND_OPCODE_DO_BIND:
					if ( address >= segmentEndAddress ) 
						throwBadBindingAddress(address, segmentEndAddress, segmentIndex, start, end, p);
					(this->*handler)(context, address, type, symbolName, symboFlags, addend, libraryOrdinal, "lazy forced", NULL);
					address += sizeof(intptr_t);
					break;
				case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
				case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
				case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
				default:
					dyld::throwf("bad lazy bind opcode %d", *p);
			}
		}
	}  

	catch (const char* msg) {
		const char* newMsg = dyld::mkstringf("%s in %s", msg, this->getPath());
		free((void*)msg);
		throw newMsg;
	}
}

// A program built targeting 10.5 will have hybrid stubs.  When used with weak symbols
// the classic lazy loader is used even when running on 10.6
uintptr_t ImageLoaderMachOCompressed::doBindLazySymbol(uintptr_t* lazyPointer, const LinkContext& context)
{
	// only works with compressed LINKEDIT if classic symbol table is also present
	const macho_nlist* symbolTable = NULL;
	const char* symbolTableStrings = NULL;
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
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	// no symbol table => no lookup by address
	if ( (symbolTable == NULL) || (dynSymbolTable == NULL) )
		dyld::throwf("classic lazy binding used with compressed LINKEDIT at %p in image %s", lazyPointer, this->getPath());

	// scan for all lazy-pointer sections
	const bool twoLevel = this->usesTwoLevelNameSpace();
	const uint32_t* const indirectTable = (uint32_t*)&fLinkEditBase[dynSymbolTable->indirectsymoff];
	cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						uint32_t symbolIndex = INDIRECT_SYMBOL_LOCAL;
						if ( type == S_LAZY_SYMBOL_POINTERS ) {
							const uint32_t pointerCount = sect->size / sizeof(uintptr_t);
							uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + fSlide);
							if ( (lazyPointer >= symbolPointers) && (lazyPointer < &symbolPointers[pointerCount]) ) {
								const uint32_t indirectTableOffset = sect->reserved1;
								const uint32_t lazyIndex = lazyPointer - symbolPointers;
								symbolIndex = indirectTable[indirectTableOffset + lazyIndex];
							}
						}
						if ( (symbolIndex != INDIRECT_SYMBOL_ABS) && (symbolIndex != INDIRECT_SYMBOL_LOCAL) ) {
							const macho_nlist* symbol = &symbolTable[symbolIndex];
							const char* symbolName = &symbolTableStrings[symbol->n_un.n_strx];
							int libraryOrdinal = GET_LIBRARY_ORDINAL(symbol->n_desc);
							if ( !twoLevel || context.bindFlat ) 
								libraryOrdinal = BIND_SPECIAL_DYLIB_FLAT_LOOKUP;
							uintptr_t ptrToBind = (uintptr_t)lazyPointer;
							uintptr_t symbolAddr = bindAt(context, ptrToBind, BIND_TYPE_POINTER, symbolName, 0, 0, libraryOrdinal, "lazy ", NULL);
							++fgTotalLazyBindFixups;
							return symbolAddr;
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	dyld::throwf("lazy pointer not found at address %p in image %s", lazyPointer, this->getPath());
}


uintptr_t ImageLoaderMachOCompressed::doBindFastLazySymbol(uint32_t lazyBindingInfoOffset, const LinkContext& context)
{
	const uint8_t* const start = fLinkEditBase + fDyldInfo->lazy_bind_off;
	const uint8_t* const end = &start[fDyldInfo->lazy_bind_size];
	if ( lazyBindingInfoOffset > fDyldInfo->lazy_bind_size )
		throw "fast lazy bind offset out of range";

	uint8_t type = BIND_TYPE_POINTER;
	uintptr_t address = 0;
	const char* symbolName = NULL;
	uint8_t symboFlags = 0;
	int libraryOrdinal = 0;
	bool done = false;
	uintptr_t result = 0;
	const uint8_t* p = &start[lazyBindingInfoOffset];
	while ( !done && (p < end) ) {
		uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
		uint8_t opcode = *p & BIND_OPCODE_MASK;
		++p;
		switch (opcode) {
			case BIND_OPCODE_DONE:
				done = true;
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
				libraryOrdinal = immediate;
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
				libraryOrdinal = read_uleb128(p, end);
				break;
			case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
				// the special ordinals are negative numbers
				if ( immediate == 0 )
					libraryOrdinal = 0;
				else {
					int8_t signExtended = BIND_OPCODE_MASK | immediate;
					libraryOrdinal = signExtended;
				}
				break;
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				symbolName = (char*)p;
				symboFlags = immediate;
				while (*p != '\0')
					++p;
				++p;
				break;
			case BIND_OPCODE_SET_TYPE_IMM:
				type = immediate;
				break;
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				if ( immediate > fSegmentsCount )
					dyld::throwf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
							immediate, fSegmentsCount);
				address = segActualLoadAddress(immediate) + read_uleb128(p, end);
				break;
			case BIND_OPCODE_DO_BIND:
				result = this->bindAt(context, address, type, symbolName, 0, 0, libraryOrdinal, "lazy ", NULL);
				break;
			case BIND_OPCODE_SET_ADDEND_SLEB:
			case BIND_OPCODE_ADD_ADDR_ULEB:
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
			default:
				dyld::throwf("bad lazy bind opcode %d", *p);
		}
	}	
	return result;
}

void ImageLoaderMachOCompressed::initializeCoalIterator(CoalIterator& it, unsigned int loadOrder)
{
	it.image = this;
	it.symbolName = " ";
	it.loadOrder = loadOrder;
	it.weakSymbol = false;
	it.symbolMatches = false;
	it.done = false;
	it.curIndex = 0;
	it.endIndex = this->fDyldInfo->weak_bind_size;
	it.address = 0;
	it.type = 0;
	it.addend = 0;
}


bool ImageLoaderMachOCompressed::incrementCoalIterator(CoalIterator& it)
{
	if ( it.done )
		return false;
		
	if ( this->fDyldInfo->weak_bind_size == 0 ) {
		/// hmmm, ld set MH_WEAK_DEFINES or MH_BINDS_TO_WEAK, but there is no weak binding info
		it.done = true;
		it.symbolName = "~~~";
		return true;
	}
	const uint8_t* start = fLinkEditBase + fDyldInfo->weak_bind_off;
	const uint8_t* p = start + it.curIndex;
	const uint8_t* end = fLinkEditBase + fDyldInfo->weak_bind_off + this->fDyldInfo->weak_bind_size;
	uint32_t count;
	uint32_t skip;
	while ( p < end ) {
		uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
		uint8_t opcode = *p & BIND_OPCODE_MASK;
		++p;
		switch (opcode) {
			case BIND_OPCODE_DONE:
				it.done = true;
				it.curIndex = p - start;
				it.symbolName = "~~~"; // sorts to end
				return true;
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				it.symbolName = (char*)p;
				it.weakSymbol = ((immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) == 0);
				it.symbolMatches = false;
				while (*p != '\0')
					++p;
				++p;
				it.curIndex = p - start;
				return false;
			case BIND_OPCODE_SET_TYPE_IMM:
				it.type = immediate;
				break;
			case BIND_OPCODE_SET_ADDEND_SLEB:
				it.addend = read_sleb128(p, end);
				break;
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				if ( immediate > fSegmentsCount )
					dyld::throwf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
							immediate, fSegmentsCount);
				it.address = segActualLoadAddress(immediate) + read_uleb128(p, end);
				break;
			case BIND_OPCODE_ADD_ADDR_ULEB:
				it.address += read_uleb128(p, end);
				break;
			case BIND_OPCODE_DO_BIND:
				it.address += sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
				it.address += read_uleb128(p, end) + sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
				it.address += immediate*sizeof(intptr_t) + sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
				count = read_uleb128(p, end);
				skip = read_uleb128(p, end);
				for (uint32_t i=0; i < count; ++i) {
					it.address += skip + sizeof(intptr_t);
				}
				break;
			default:
				dyld::throwf("bad weak bind opcode %d", *p);
		}
	}
	/// hmmm, BIND_OPCODE_DONE is missing...
	it.done = true;
	it.symbolName = "~~~";
	//dyld::log("missing BIND_OPCODE_DONE for image %s\n", this->getPath());
	return true;
}

uintptr_t ImageLoaderMachOCompressed::getAddressCoalIterator(CoalIterator& it, const LinkContext& context)
{
	//dyld::log("looking for %s in %s\n", it.symbolName, this->getPath());
	const ImageLoader* foundIn = NULL;
	const ImageLoader::Symbol* sym = this->findExportedSymbol(it.symbolName, &foundIn);
	if ( sym != NULL ) {
		//dyld::log("sym=%p, foundIn=%p\n", sym, foundIn);
		return foundIn->getExportedSymbolAddress(sym, context, this);
	}
	return 0;
}


void ImageLoaderMachOCompressed::updateUsesCoalIterator(CoalIterator& it, uintptr_t value, ImageLoader* targetImage, const LinkContext& context)
{
	// <rdar://problem/6570879> weak binding done too early with inserted libraries
	if ( this->getState() < dyld_image_state_bound  )
		return;

	const uint8_t* start = fLinkEditBase + fDyldInfo->weak_bind_off;
	const uint8_t* p = start + it.curIndex;
	const uint8_t* end = fLinkEditBase + fDyldInfo->weak_bind_off + this->fDyldInfo->weak_bind_size;

	uint8_t type = it.type;
	uintptr_t address = it.address;
	const char* symbolName = it.symbolName;
	intptr_t addend = it.addend;
	uint32_t count;
	uint32_t skip;
	bool done = false;
	bool boundSomething = false;
	while ( !done && (p < end) ) {
		uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
		uint8_t opcode = *p & BIND_OPCODE_MASK;
		++p;
		switch (opcode) {
			case BIND_OPCODE_DONE:
				done = true;
				break;
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				done = true;
				break;
			case BIND_OPCODE_SET_TYPE_IMM:
				type = immediate;
				break;
			case BIND_OPCODE_SET_ADDEND_SLEB:
				addend = read_sleb128(p, end);
				break;
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				if ( immediate > fSegmentsCount )
					dyld::throwf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB has segment %d which is too large (%d)\n", 
							immediate, fSegmentsCount);
				address = segActualLoadAddress(immediate) + read_uleb128(p, end);
				break;
			case BIND_OPCODE_ADD_ADDR_ULEB:
				address += read_uleb128(p, end);
				break;
			case BIND_OPCODE_DO_BIND:
				bindLocation(context, address, value, targetImage, type, symbolName, addend, "weak ");
				boundSomething = true;
				address += sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
				bindLocation(context, address, value, targetImage, type, symbolName, addend, "weak ");
				boundSomething = true;
				address += read_uleb128(p, end) + sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
				bindLocation(context, address, value, targetImage, type, symbolName, addend, "weak ");
				boundSomething = true;
				address += immediate*sizeof(intptr_t) + sizeof(intptr_t);
				break;
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
				count = read_uleb128(p, end);
				skip = read_uleb128(p, end);
				for (uint32_t i=0; i < count; ++i) {
					bindLocation(context, address, value, targetImage, type, symbolName, addend, "weak ");
					boundSomething = true;
					address += skip + sizeof(intptr_t);
				}
				break;
			default:
				dyld::throwf("bad bind opcode %d in weak binding info", *p);
		}
	}	
	if ( boundSomething && (targetImage != this) && !targetImage->neverUnload() )
		this->addDynamicReference(targetImage);
}





const char* ImageLoaderMachOCompressed::findClosestSymbol(const void* addr, const void** closestAddr) const
{
	// called by dladdr()
	// only works with compressed LINKEDIT if classic symbol table is also present
	const macho_nlist* symbolTable = NULL;
	const char* symbolTableStrings = NULL;
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
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	// no symbol table => no lookup by address
	if ( (symbolTable == NULL) || (dynSymbolTable == NULL) )
		return NULL;

	uintptr_t targetAddress = (uintptr_t)addr - fSlide;
	const struct macho_nlist* bestSymbol = NULL;
	// first walk all global symbols
	const struct macho_nlist* const globalsStart = &symbolTable[dynSymbolTable->iextdefsym];
	const struct macho_nlist* const globalsEnd= &globalsStart[dynSymbolTable->nextdefsym];
	for (const struct macho_nlist* s = globalsStart; s < globalsEnd; ++s) {
 		if ( (s->n_type & N_TYPE) == N_SECT ) {
			if ( bestSymbol == NULL ) {
				if ( s->n_value <= targetAddress )
					bestSymbol = s;
			}
			else if ( (s->n_value <= targetAddress) && (bestSymbol->n_value < s->n_value) ) {
				bestSymbol = s;
			}
		}
	}
	// next walk all local symbols
	const struct macho_nlist* const localsStart = &symbolTable[dynSymbolTable->ilocalsym];
	const struct macho_nlist* const localsEnd= &localsStart[dynSymbolTable->nlocalsym];
	for (const struct macho_nlist* s = localsStart; s < localsEnd; ++s) {
 		if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
			if ( bestSymbol == NULL ) {
				if ( s->n_value <= targetAddress )
					bestSymbol = s;
			}
			else if ( (s->n_value <= targetAddress) && (bestSymbol->n_value < s->n_value) ) {
				bestSymbol = s;
			}
		}
	}
	if ( bestSymbol != NULL ) {
		*closestAddr = (void*)(bestSymbol->n_value + fSlide);
		return &symbolTableStrings[bestSymbol->n_un.n_strx];
	}
	return NULL;
}


#if PREBOUND_IMAGE_SUPPORT
void ImageLoaderMachOCompressed::resetPreboundLazyPointers(const LinkContext& context)
{
	// no way to back off a prebound compress image
}
#endif

