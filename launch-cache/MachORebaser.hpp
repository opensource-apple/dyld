/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006 Apple Computer, Inc. All rights reserved.
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

#ifndef __MACHO_REBASER__
#define __MACHO_REBASER__

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/reloc.h>
#include <mach-o/ppc/reloc.h>
#include <mach-o/x86_64/reloc.h>
#include <mach-o/arm/reloc.h>
#include <vector>
#include <set>

#include "MachOFileAbstraction.hpp"
#include "Architectures.hpp"
#include "MachOLayout.hpp"
#include "MachOTrie.hpp"



class AbstractRebaser
{
public:
	virtual cpu_type_t							getArchitecture() const = 0;
	virtual uint64_t							getBaseAddress() const = 0;
	virtual uint64_t							getVMSize() const = 0;
	virtual void								rebase(std::vector<void*>&) = 0;
};


template <typename A>
class Rebaser : public AbstractRebaser
{
public:
												Rebaser(const MachOLayoutAbstraction&);
	virtual										~Rebaser() {}

	virtual cpu_type_t							getArchitecture() const;
	virtual uint64_t							getBaseAddress() const;
	virtual uint64_t							getVMSize() const;
	virtual void								rebase(std::vector<void*>&);

protected:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
		
	pint_t*										mappedAddressForNewAddress(pint_t vmaddress);
	pint_t										getSlideForNewAddress(pint_t newAddress);
	
private:
	void										calculateRelocBase();
	void										adjustLoadCommands();
	void										adjustSymbolTable();
	void										optimzeStubs();
	void										makeNoPicStub(uint8_t* stub, pint_t logicalAddress);
	void										adjustDATA();
	void										adjustCode();
	void										applyRebaseInfo(std::vector<void*>& pointersInData);
	void										adjustExportInfo();
	void										doRebase(int segIndex, uint64_t segOffset, uint8_t type, std::vector<void*>& pointersInData);
	void										adjustSegmentLoadCommand(macho_segment_command<P>* seg);
	pint_t										getSlideForVMAddress(pint_t vmaddress);
	pint_t*										mappedAddressForVMAddress(pint_t vmaddress);
	pint_t*										mappedAddressForRelocAddress(pint_t r_address);
	void										adjustRelocBaseAddresses();
	const uint8_t*								doCodeUpdateForEachULEB128Address(const uint8_t* p, uint8_t kind, uint64_t orgBaseAddress, int64_t codeToDataDelta, int64_t codeToImportDelta);
	void										doCodeUpdate(uint8_t kind, uint64_t address, int64_t codeToDataDelta, int64_t codeToImportDelta);
	void										doLocalRelocation(const macho_relocation_info<P>* reloc);
	bool										unequalSlides() const;

protected:	
	const macho_header<P>*						fHeader; 
	uint8_t*									fLinkEditBase;				// add file offset to this to get linkedit content
	const MachOLayoutAbstraction&				fLayout;
private:
	pint_t										fOrignalVMRelocBaseAddress; // add reloc address to this to get original address reloc referred to
	const macho_symtab_command<P>*				fSymbolTable;
	const macho_dysymtab_command<P>*			fDynamicSymbolTable;
	const macho_dyld_info_command<P>*			fDyldInfo;
	bool										fSplittingSegments;
	bool										fOrignalVMRelocBaseAddressValid;
	pint_t										fSkipSplitSegInfoStart;
	pint_t										fSkipSplitSegInfoEnd;
};



template <typename A>
Rebaser<A>::Rebaser(const MachOLayoutAbstraction& layout)
 : 	fLayout(layout), fOrignalVMRelocBaseAddress(NULL), fLinkEditBase(NULL), 
	fSymbolTable(NULL), fDynamicSymbolTable(NULL), fDyldInfo(NULL), fSplittingSegments(false), 
	fOrignalVMRelocBaseAddressValid(false), fSkipSplitSegInfoStart(0), fSkipSplitSegInfoEnd(0)
{
	fHeader = (const macho_header<P>*)fLayout.getSegments()[0].mappedAddress();
	switch ( fHeader->filetype() ) {
		case MH_DYLIB:
		case MH_BUNDLE:
			break;
		default:
			throw "file is not a dylib or bundle";
	}
	
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( strcmp(seg.name(), "__LINKEDIT") == 0 ) {
			fLinkEditBase = (uint8_t*)seg.mappedAddress() - seg.fileOffset();
			break;
		}
	}
	if ( fLinkEditBase == NULL )	
		throw "no __LINKEDIT segment";
		
	// get symbol table info
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_SYMTAB:
				fSymbolTable = (macho_symtab_command<P>*)cmd;
				break;
			case LC_DYSYMTAB:
				fDynamicSymbolTable = (macho_dysymtab_command<P>*)cmd;
				break;
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				fDyldInfo = (macho_dyld_info_command<P>*)cmd;
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	

	calculateRelocBase();
	
	fSplittingSegments = layout.hasSplitSegInfo() && this->unequalSlides();
}

template <> cpu_type_t Rebaser<ppc>::getArchitecture()    const { return CPU_TYPE_POWERPC; }
template <> cpu_type_t Rebaser<x86>::getArchitecture()    const { return CPU_TYPE_I386; }
template <> cpu_type_t Rebaser<x86_64>::getArchitecture() const { return CPU_TYPE_X86_64; }
template <> cpu_type_t Rebaser<arm>::getArchitecture() const { return CPU_TYPE_ARM; }

template <typename A>
bool Rebaser<A>::unequalSlides() const
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	uint64_t slide = segments[0].newAddress() - segments[0].address();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( (seg.newAddress() - seg.address()) != slide )
			return true;
	}
	return false;
}

template <typename A>
uint64_t Rebaser<A>::getBaseAddress() const
{
	return fLayout.getSegments()[0].address();
}

template <typename A>
uint64_t Rebaser<A>::getVMSize() const
{
	uint64_t highestVMAddress = 0;
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( seg.address() > highestVMAddress )
			highestVMAddress = seg.address();
	}
	return (((highestVMAddress - getBaseAddress()) + 4095) & (-4096));
}



template <typename A>
void Rebaser<A>::rebase(std::vector<void*>& pointersInData)
{		
	// update writable segments that have internal pointers
	if ( fDyldInfo != NULL )
		this->applyRebaseInfo(pointersInData);
	else
		this->adjustDATA();

	// if splitting segments, update code-to-data references
	this->adjustCode();
	
	// change address on relocs now that segments are split
	this->adjustRelocBaseAddresses();
	
	// update load commands
	this->adjustLoadCommands();
	
	// update symbol table  
	this->adjustSymbolTable();
	
	// optimize stubs 
	this->optimzeStubs();
	
	// update export info
	if ( fDyldInfo != NULL )
		this->adjustExportInfo();
}

template <>
void Rebaser<x86>::adjustSegmentLoadCommand(macho_segment_command<P>* seg)
{
	// __IMPORT segments are not-writable in shared cache
	if ( strcmp(seg->segname(), "__IMPORT") == 0 ) 
		seg->set_initprot(VM_PROT_READ|VM_PROT_EXECUTE);
}

template <typename A>
void Rebaser<A>::adjustSegmentLoadCommand(macho_segment_command<P>* seg)
{
}


template <typename A>
void Rebaser<A>::adjustLoadCommands()
{
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch ( cmd->cmd() ) {
			case LC_ID_DYLIB:
				if ( (fHeader->flags() & MH_PREBOUND) != 0 ) {
					// clear timestamp so that any prebound clients are invalidated
					macho_dylib_command<P>* dylib  = (macho_dylib_command<P>*)cmd;
					dylib->set_timestamp(1);
				}
				break;
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				if ( (fHeader->flags() & MH_PREBOUND) != 0 ) {
					// clear expected timestamps so that this image will load with invalid prebinding 
					macho_dylib_command<P>* dylib  = (macho_dylib_command<P>*)cmd;
					dylib->set_timestamp(2);
				}
				break;
			case macho_routines_command<P>::CMD:
				// update -init command
				{
					struct macho_routines_command<P>* routines = (struct macho_routines_command<P>*)cmd;
					routines->set_init_address(routines->init_address() + this->getSlideForVMAddress(routines->init_address()));
				}
				break;
			case macho_segment_command<P>::CMD:
				// update segment commands
				{
					macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
					this->adjustSegmentLoadCommand(seg);
					pint_t slide = this->getSlideForVMAddress(seg->vmaddr());
					seg->set_vmaddr(seg->vmaddr() + slide);
					macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
					macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
					for(macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
						sect->set_addr(sect->addr() + slide);
					}
				}
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}



template <typename A>
typename A::P::uint_t Rebaser<A>::getSlideForVMAddress(pint_t vmaddress)
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( (seg.address() <= vmaddress) && (seg.size() != 0) && ((vmaddress < (seg.address()+seg.size())) || (seg.address() == vmaddress)) ) {
			return seg.newAddress() - seg.address();
		}
	}
	throwf("vm address 0x%08llX not found", (uint64_t)vmaddress);
}


template <typename A>
typename A::P::uint_t* Rebaser<A>::mappedAddressForVMAddress(pint_t vmaddress)
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( (seg.address() <= vmaddress) && (vmaddress < (seg.address()+seg.size())) ) {
			return (pint_t*)((vmaddress - seg.address()) + (uint8_t*)seg.mappedAddress());
		}
	}
	throwf("mappedAddressForVMAddress(0x%08llX) not found", (uint64_t)vmaddress);
}

template <typename A>
typename A::P::uint_t* Rebaser<A>::mappedAddressForNewAddress(pint_t vmaddress)
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( (seg.newAddress() <= vmaddress) && (vmaddress < (seg.newAddress()+seg.size())) ) {
			return (pint_t*)((vmaddress - seg.newAddress()) + (uint8_t*)seg.mappedAddress());
		}
	}
	throwf("mappedAddressForNewAddress(0x%08llX) not found", (uint64_t)vmaddress);
}

template <typename A>
typename A::P::uint_t Rebaser<A>::getSlideForNewAddress(pint_t newAddress)
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( (seg.newAddress() <= newAddress) && (newAddress < (seg.newAddress()+seg.size())) ) {
			return seg.newAddress() - seg.address();
		}
	}
	throwf("new address 0x%08llX not found", (uint64_t)newAddress);
}

template <typename A>
typename A::P::uint_t* Rebaser<A>::mappedAddressForRelocAddress(pint_t r_address)
{
	if ( fOrignalVMRelocBaseAddressValid )
		return this->mappedAddressForVMAddress(r_address + fOrignalVMRelocBaseAddress);
	else
		throw "can't apply relocation.  Relocation base not known";
}


template <>
void Rebaser<arm>::makeNoPicStub(uint8_t* stub, pint_t logicalAddress) 
{
	uint32_t* instructions = (uint32_t*)stub;
	if ( (LittleEndian::get32(instructions[0]) == 0xE59FC004) && 
		(LittleEndian::get32(instructions[1]) == 0xE08FC00C) && 
		(LittleEndian::get32(instructions[2]) == 0xE59CF000) ) {
			uint32_t lazyPtrAddress = instructions[3] + logicalAddress + 12;
			LittleEndian::set32(instructions[0], 0xE59FC000);		// 	ldr ip, [pc, #0]
			LittleEndian::set32(instructions[1], 0xE59CF000);		// 	ldr pc, [ip]
			LittleEndian::set32(instructions[2], lazyPtrAddress);	// 	.long   L_foo$lazy_ptr
			LittleEndian::set32(instructions[3], 0xE1A00000);		// 	nop
	}
	else
		fprintf(stderr, "unoptimized stub in %s at 0x%08X\n", fLayout.getFilePath(), logicalAddress);
}


#if 0 
// disable this optimization do allow cache to slide
template <>
void Rebaser<arm>::optimzeStubs()
{
	// convert pic stubs to no-pic stubs in dyld shared cache
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
			macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
			for(macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
				if ( (sect->flags() & SECTION_TYPE) == S_SYMBOL_STUBS ) {
					const uint32_t stubSize = sect->reserved2();
					// ARM PIC stubs are 4 32-bit instructions long
					if ( stubSize == 16 ) {
						uint32_t stubCount = sect->size() / 16;
						pint_t stubLogicalAddress = sect->addr();
						uint8_t* stubMappedAddress = (uint8_t*)mappedAddressForNewAddress(stubLogicalAddress);
						for(uint32_t s=0; s < stubCount; ++s) {
							makeNoPicStub(stubMappedAddress, stubLogicalAddress);
							stubLogicalAddress += 16;
							stubMappedAddress += 16;
						}
					}
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}
#endif

template <typename A>
void Rebaser<A>::optimzeStubs()
{
	// other architectures don't need stubs changed in shared cache
}

template <typename A>
void Rebaser<A>::adjustSymbolTable()
{
	macho_nlist<P>* symbolTable = (macho_nlist<P>*)(&fLinkEditBase[fSymbolTable->symoff()]);

	// walk all exports and slide their n_value
	macho_nlist<P>* lastExport = &symbolTable[fDynamicSymbolTable->iextdefsym()+fDynamicSymbolTable->nextdefsym()];
	for (macho_nlist<P>* entry = &symbolTable[fDynamicSymbolTable->iextdefsym()]; entry < lastExport; ++entry) {
		if ( (entry->n_type() & N_TYPE) == N_SECT )
			entry->set_n_value(entry->n_value() + this->getSlideForVMAddress(entry->n_value()));
	}

	// walk all local symbols and slide their n_value (don't adjust any stabs)
	macho_nlist<P>*  lastLocal = &symbolTable[fDynamicSymbolTable->ilocalsym()+fDynamicSymbolTable->nlocalsym()];
	for (macho_nlist<P>* entry = &symbolTable[fDynamicSymbolTable->ilocalsym()]; entry < lastLocal; ++entry) {
		if ( (entry->n_sect() != NO_SECT) && ((entry->n_type() & N_STAB) == 0) )
			entry->set_n_value(entry->n_value() + this->getSlideForVMAddress(entry->n_value()));
	}
}

template <typename A>
void Rebaser<A>::adjustExportInfo()
{
	// if no export info, nothing to adjust
	if ( fDyldInfo->export_size() == 0 )
		return;

	// since export info addresses are offsets from mach_header, everything in __TEXT is fine
	// only __DATA addresses need to be updated
	const uint8_t* start = fLayout.getDyldInfoExports(); 
	const uint8_t* end = &start[fDyldInfo->export_size()];
	std::vector<mach_o::trie::Entry> originalExports;
	try {
		parseTrie(start, end, originalExports);
	}
	catch (const char* msg) {
		throwf("%s in %s", msg, fLayout.getFilePath());
	}
	
	std::vector<mach_o::trie::Entry> newExports;
	newExports.reserve(originalExports.size());
	pint_t baseAddress = this->getBaseAddress();
	pint_t baseAddressSlide = this->getSlideForVMAddress(baseAddress);
	for (std::vector<mach_o::trie::Entry>::iterator it=originalExports.begin(); it != originalExports.end(); ++it) {
		// remove symbols used by the static linker only
		if (	   (strncmp(it->name, "$ld$", 4) == 0) 
				|| (strncmp(it->name, ".objc_class_name",16) == 0) 
				|| (strncmp(it->name, ".objc_category_name",19) == 0) ) {
			//fprintf(stderr, "ignoring symbol %s\n", it->name);
			continue;
		}
		// adjust symbols in slid segments
		//uint32_t oldOffset = it->address;
		it->address += (this->getSlideForVMAddress(it->address + baseAddress) - baseAddressSlide);
		//fprintf(stderr, "orig=0x%08X, new=0x%08llX, sym=%s\n", oldOffset, it->address, it->name);
		newExports.push_back(*it);
	}
	
	// rebuild export trie
	std::vector<uint8_t> newExportTrieBytes;
	newExportTrieBytes.reserve(fDyldInfo->export_size());
	mach_o::trie::makeTrie(newExports, newExportTrieBytes);
	// align
	while ( (newExportTrieBytes.size() % sizeof(pint_t)) != 0 )
		newExportTrieBytes.push_back(0);
	
	// allocate new buffer and set export_off to use new buffer instead
	uint32_t newExportsSize = newExportTrieBytes.size();
	uint8_t* sideTrie = new uint8_t[newExportsSize];
	memcpy(sideTrie, &newExportTrieBytes[0], newExportsSize);
	fLayout.setDyldInfoExports(sideTrie);
	((macho_dyld_info_command<P>*)fDyldInfo)->set_export_off(0); // invalidate old trie
	((macho_dyld_info_command<P>*)fDyldInfo)->set_export_size(newExportsSize);
}



template <typename A>
void Rebaser<A>::doCodeUpdate(uint8_t kind, uint64_t address, int64_t codeToDataDelta, int64_t codeToImportDelta)
{
	// begin hack for <rdar://problem/8253549> split seg info wrong for x86_64 stub helpers
	if ( (fSkipSplitSegInfoStart <= address) && (address < fSkipSplitSegInfoEnd) ) {
		uint8_t* p = (uint8_t*)mappedAddressForVMAddress(address);
		// only ignore split seg info for "push" instructions
		if ( p[-1] == 0x68 )
			return;
	}
	// end hack for <rdar://problem/8253549> 
	
	//fprintf(stderr, "doCodeUpdate(kind=%d, address=0x%0llX, dataDelta=0x%08llX, importDelta=0x%08llX, path=%s)\n", 
	//				kind, address, codeToDataDelta, codeToImportDelta, fLayout.getFilePath());
	uint32_t* p;
	uint32_t instruction;
	uint32_t value;
	uint64_t value64;
	switch (kind) {
		case 1:	// 32-bit pointer
			p = (uint32_t*)mappedAddressForVMAddress(address);
			value = A::P::E::get32(*p);
			value += codeToDataDelta;
			 A::P::E::set32(*p, value);
			break;
		case 2: // 64-bit pointer
			p = (uint32_t*)mappedAddressForVMAddress(address);
			value64 =  A::P::E::get64(*(uint64_t*)p);
			value64 += codeToDataDelta;
			 A::P::E::set64(*(uint64_t*)p, value64);
			break;
		case 3: // used only for ppc, an instruction that sets the hi16 of a register
			// adjust low 16 bits of instruction which contain hi16 of distance to something in DATA
			if ( (codeToDataDelta & 0xFFFF) != 0 )
				throwf("codeToDataDelta=0x%0llX is not a multiple of 64K", codeToDataDelta);
			p = (uint32_t*)mappedAddressForVMAddress(address);
			instruction = BigEndian::get32(*p);
			{
				uint16_t originalLo16 = instruction & 0x0000FFFF;
				uint16_t delta64Ks = codeToDataDelta >> 16;
				instruction = (instruction & 0xFFFF0000) | ((originalLo16+delta64Ks) & 0x0000FFFF);
			}
			BigEndian::set32(*p, instruction);
			break;
		case 4:	// only used for i386, a reference to something in the IMPORT segment
			p = (uint32_t*)mappedAddressForVMAddress(address);
			value = A::P::E::get32(*p);
			value += codeToImportDelta;
			 A::P::E::set32(*p, value);
			break;
		default:
			throwf("invalid kind=%d in split seg info", kind);
	}
}

template <typename A>
const uint8_t* Rebaser<A>::doCodeUpdateForEachULEB128Address(const uint8_t* p, uint8_t kind, uint64_t orgBaseAddress, int64_t codeToDataDelta, int64_t codeToImportDelta)
{
	uint64_t address = 0;
	uint64_t delta = 0;
	uint32_t shift = 0;
	bool more = true;
	do {
		uint8_t byte = *p++;
		delta |= ((byte & 0x7F) << shift);
		shift += 7;
		if ( byte < 0x80 ) {
			if ( delta != 0 ) {
				address += delta;
				doCodeUpdate(kind, address+orgBaseAddress, codeToDataDelta, codeToImportDelta);
				delta = 0;
				shift = 0;
			}
			else {
				more = false;
			}
		}
	} while (more);
	return p;
}

template <typename A>
void Rebaser<A>::adjustCode()
{
	if ( fSplittingSegments ) {
		// get uleb128 compressed runs of code addresses to update
		const uint8_t* infoStart = NULL;
		const uint8_t* infoEnd = NULL;
		const macho_segment_command<P>* seg;
		const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
		const uint32_t cmd_count = fHeader->ncmds();
		const macho_load_command<P>* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd()) {
				case LC_SEGMENT_SPLIT_INFO:
					{
						const macho_linkedit_data_command<P>* segInfo = (macho_linkedit_data_command<P>*)cmd;
						infoStart = &fLinkEditBase[segInfo->dataoff()];
						infoEnd = &infoStart[segInfo->datasize()];
					}
					break;
				// begin hack for <rdar://problem/8253549> split seg info wrong for x86_64 stub helpers
				case macho_segment_command<P>::CMD:
					seg = (macho_segment_command<P>*)cmd;
					if ( (getArchitecture() == CPU_TYPE_X86_64) && (strcmp(seg->segname(), "__TEXT") == 0) ) {
						const macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
						const macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
						for(const macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
							if ( strcmp(sect->sectname(), "__stub_helper") == 0 ) {
								fSkipSplitSegInfoStart = sect->addr();
								fSkipSplitSegInfoEnd = sect->addr() + sect->size() - 16;
							}	
						}
					}
					break;
				// end hack for <rdar://problem/8253549> split seg info wrong for x86_64 stub helpers
			}
			cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
		}
		// calculate how much we need to slide writable segments
		const uint64_t orgBaseAddress = this->getBaseAddress();
		int64_t codeToDataDelta = 0;
		int64_t codeToImportDelta = 0;
		const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
		const MachOLayoutAbstraction::Segment& codeSeg = segments[0];
		for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
			const MachOLayoutAbstraction::Segment& dataSeg = *it;
			if ( strcmp(dataSeg.name(), "__IMPORT") == 0 )
				codeToImportDelta = (dataSeg.newAddress() - codeSeg.newAddress()) - (dataSeg.address() - codeSeg.address());
			else if ( dataSeg.writable() ) 
				codeToDataDelta = (dataSeg.newAddress() - codeSeg.newAddress()) - (dataSeg.address() - codeSeg.address());
		}
		// decompress and call doCodeUpdate() on each address
		for(const uint8_t* p = infoStart; (*p != 0) && (p < infoEnd);) {
			uint8_t kind = *p++;
			p = this->doCodeUpdateForEachULEB128Address(p, kind, orgBaseAddress, codeToDataDelta, codeToImportDelta);
		}
	}
}

template <typename A>
void Rebaser<A>::doRebase(int segIndex, uint64_t segOffset, uint8_t type, std::vector<void*>& pointersInData)
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	if ( segIndex > segments.size() )
		throw "bad segment index in rebase info";
	const MachOLayoutAbstraction::Segment& seg = segments[segIndex];
	uint8_t*  mappedAddr = (uint8_t*)seg.mappedAddress() + segOffset;
	pint_t*   mappedAddrP = (pint_t*)mappedAddr;
	uint32_t* mappedAddr32 = (uint32_t*)mappedAddr;
	pint_t valueP;
	pint_t valuePnew;
	uint32_t value32;
	int32_t svalue32;
	int32_t svalue32new;
	switch ( type ) {
		case REBASE_TYPE_POINTER:
			valueP= P::getP(*mappedAddrP);
			P::setP(*mappedAddrP, valueP + this->getSlideForVMAddress(valueP));
			break;
		
		case REBASE_TYPE_TEXT_ABSOLUTE32:
			value32 = E::get32(*mappedAddr32);
			E::set32(*mappedAddr32, value32 + this->getSlideForVMAddress(value32));
			break;
			
		case REBASE_TYPE_TEXT_PCREL32:
			svalue32 = E::get32(*mappedAddr32);
			valueP = seg.address() + segOffset + 4 + svalue32;
			valuePnew = valueP + this->getSlideForVMAddress(valueP);
			svalue32new = seg.address() + segOffset + 4 - valuePnew;
			E::set32(*mappedAddr32, svalue32new);
			break;
		
		default:
			throw "bad rebase type";
	}
	pointersInData.push_back(mappedAddr);
}


template <typename A>
void Rebaser<A>::applyRebaseInfo(std::vector<void*>& pointersInData)
{
	const uint8_t* p = &fLinkEditBase[fDyldInfo->rebase_off()];
	const uint8_t* end = &p[fDyldInfo->rebase_size()];
	
	uint8_t type = 0;
	int segIndex;
	uint64_t segOffset = 0;
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
				segIndex = immediate;
				segOffset = read_uleb128(p, end);
				break;
			case REBASE_OPCODE_ADD_ADDR_ULEB:
				segOffset += read_uleb128(p, end);
				break;
			case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
				segOffset += immediate*sizeof(pint_t);
				break;
			case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
				for (int i=0; i < immediate; ++i) {
					doRebase(segIndex, segOffset, type, pointersInData);
					segOffset += sizeof(pint_t);
				}
				break;
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
				count = read_uleb128(p, end);
				for (uint32_t i=0; i < count; ++i) {
					doRebase(segIndex, segOffset, type, pointersInData);
					segOffset += sizeof(pint_t);
				}
				break;
			case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
				doRebase(segIndex, segOffset, type, pointersInData);
				segOffset += read_uleb128(p, end) + sizeof(pint_t);
				break;
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
				count = read_uleb128(p, end);
				skip = read_uleb128(p, end);
				for (uint32_t i=0; i < count; ++i) {
					doRebase(segIndex, segOffset, type, pointersInData);
					segOffset += skip + sizeof(pint_t);
				}
				break;
			default:
				throwf("bad rebase opcode %d", *p);
		}
	}	
}

template <typename A>
void Rebaser<A>::adjustDATA()
{
	// walk all local relocations and slide every pointer
	const macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[fDynamicSymbolTable->locreloff()]);
	const macho_relocation_info<P>* const relocsEnd = &relocsStart[fDynamicSymbolTable->nlocrel()];
	for (const macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		this->doLocalRelocation(reloc);
	}
	
	// walk non-lazy-pointers and slide the ones that are LOCAL
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			const macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			const macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
			const macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
			const uint32_t* const indirectTable = (uint32_t*)(&fLinkEditBase[fDynamicSymbolTable->indirectsymoff()]);
			for(const macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
				if ( (sect->flags() & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS ) {
					const uint32_t indirectTableOffset = sect->reserved1();
					uint32_t pointerCount = sect->size() / sizeof(pint_t);
					pint_t* nonLazyPointerAddr = this->mappedAddressForVMAddress(sect->addr());
					for (uint32_t j=0; j < pointerCount; ++j, ++nonLazyPointerAddr) {
						if ( E::get32(indirectTable[indirectTableOffset + j]) == INDIRECT_SYMBOL_LOCAL ) {
							pint_t value = A::P::getP(*nonLazyPointerAddr);
							P::setP(*nonLazyPointerAddr, value + this->getSlideForVMAddress(value));
						}
					}
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
}


template <typename A>
void Rebaser<A>::adjustRelocBaseAddresses()
{
	// split seg file need reloc base to be first writable segment
	if ( fSplittingSegments && ((fHeader->flags() & MH_SPLIT_SEGS) == 0) ) {

		// get amount to adjust reloc address
		int32_t relocAddressAdjust = 0;
		const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
		for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
			const MachOLayoutAbstraction::Segment& seg = *it;
			if ( seg.writable() ) {
				relocAddressAdjust = seg.address() - segments[0].address();
				break;
			}
		}

		// walk all local relocations and adjust every address 
		macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[fDynamicSymbolTable->locreloff()]);
		macho_relocation_info<P>* const relocsEnd = &relocsStart[fDynamicSymbolTable->nlocrel()];
		for (macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
			reloc->set_r_address(reloc->r_address()-relocAddressAdjust);
		}
		
		// walk all external relocations and adjust every address 
		macho_relocation_info<P>* const externRelocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[fDynamicSymbolTable->extreloff()]);
		macho_relocation_info<P>* const externRelocsEnd = &externRelocsStart[fDynamicSymbolTable->nextrel()];
		for (macho_relocation_info<P>* reloc=externRelocsStart; reloc < externRelocsEnd; ++reloc) {
			reloc->set_r_address(reloc->r_address()-relocAddressAdjust);
		}
	}
}

template <>
void Rebaser<x86_64>::adjustRelocBaseAddresses()
{
	// x86_64 already have reloc base of first writable segment
}


template <>
void Rebaser<x86_64>::doLocalRelocation(const macho_relocation_info<x86_64::P>* reloc)
{
	if ( reloc->r_type() == X86_64_RELOC_UNSIGNED ) {
		pint_t* addr = this->mappedAddressForRelocAddress(reloc->r_address());
		pint_t value = P::getP(*addr);
		P::setP(*addr, value + this->getSlideForVMAddress(value));
	}
	else {
		throw "invalid relocation type";
	}
}

template <>
void Rebaser<ppc>::doLocalRelocation(const macho_relocation_info<P>* reloc)
{
	if ( (reloc->r_address() & R_SCATTERED) == 0 ) {
		if ( reloc->r_type() == GENERIC_RELOC_VANILLA ) {
			pint_t* addr = this->mappedAddressForRelocAddress(reloc->r_address());
			pint_t value = P::getP(*addr);
			P::setP(*addr, value + this->getSlideForVMAddress(value));
		}
	}
	else {
		macho_scattered_relocation_info<P>* sreloc = (macho_scattered_relocation_info<P>*)reloc;
		if ( sreloc->r_type() == PPC_RELOC_PB_LA_PTR ) {
			sreloc->set_r_value( sreloc->r_value() + this->getSlideForVMAddress(sreloc->r_value()) );
		}
		else {
			throw "cannot rebase final linked image with scattered relocations";
		}
	}
}

template <>
void Rebaser<x86>::doLocalRelocation(const macho_relocation_info<P>* reloc)
{
	if ( (reloc->r_address() & R_SCATTERED) == 0 ) {
		if ( reloc->r_type() == GENERIC_RELOC_VANILLA ) {
			pint_t* addr = this->mappedAddressForRelocAddress(reloc->r_address());
			pint_t value = P::getP(*addr);
			P::setP(*addr, value + this->getSlideForVMAddress(value));
		}
	}
	else {
		macho_scattered_relocation_info<P>* sreloc = (macho_scattered_relocation_info<P>*)reloc;
		if ( sreloc->r_type() == GENERIC_RELOC_PB_LA_PTR ) {
			sreloc->set_r_value( sreloc->r_value() + this->getSlideForVMAddress(sreloc->r_value()) );
		}
		else {
			throw "cannot rebase final linked image with scattered relocations";
		}
	}
}

template <typename A>
void Rebaser<A>::doLocalRelocation(const macho_relocation_info<P>* reloc)
{
	if ( (reloc->r_address() & R_SCATTERED) == 0 ) {
		if ( reloc->r_type() == GENERIC_RELOC_VANILLA ) {
			pint_t* addr = this->mappedAddressForRelocAddress(reloc->r_address());
			pint_t value = P::getP(*addr);
			P::setP(*addr, value + this->getSlideForVMAddress(value));
		}
	}
	else {
		throw "cannot rebase final linked image with scattered relocations";
	}
}


template <typename A>
void Rebaser<A>::calculateRelocBase()
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	if ( fHeader->flags() & MH_SPLIT_SEGS ) {
		// reloc addresses are from the start of the first writable segment
		for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
			const MachOLayoutAbstraction::Segment& seg = *it;
			if ( seg.writable() ) {
				// found first writable segment
				fOrignalVMRelocBaseAddress = seg.address();
				fOrignalVMRelocBaseAddressValid = true;
			}
		}
	}
	else {
		// reloc addresses are from the start of the mapped file (base address)
		fOrignalVMRelocBaseAddress = segments[0].address();
		fOrignalVMRelocBaseAddressValid = true;
	}
}


template <>
void Rebaser<x86_64>::calculateRelocBase()
{
	// reloc addresses are always based from the start of the first writable segment
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( seg.writable() ) {
			// found first writable segment
			fOrignalVMRelocBaseAddress = seg.address();
			fOrignalVMRelocBaseAddressValid = true;
		}
	}
}


#if 0
class MultiArchRebaser
{
public:				
		MultiArchRebaser::MultiArchRebaser(const char* path, bool writable=false)
		 : fMappingAddress(0), fFileSize(0)
		{
			// map in whole file
			int fd = ::open(path, (writable ? O_RDWR : O_RDONLY), 0);
			if ( fd == -1 )
				throwf("can't open file, errno=%d", errno);
			struct stat stat_buf;
			if ( fstat(fd, &stat_buf) == -1)
				throwf("can't stat open file %s, errno=%d", path, errno);
			if ( stat_buf.st_size < 20 )
				throwf("file too small %s", path);
			const int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
			const int flags = writable ? (MAP_FILE | MAP_SHARED) : (MAP_FILE | MAP_PRIVATE);
			uint8_t* p = (uint8_t*)::mmap(NULL, stat_buf.st_size, prot, flags, fd, 0);
			if ( p == (uint8_t*)(-1) )
				throwf("can't map file %s, errno=%d", path, errno);
			::close(fd);

			// if fat file, process each architecture
			const fat_header* fh = (fat_header*)p;
			const mach_header* mh = (mach_header*)p;
			if ( fh->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
				// Fat header is always big-endian
				const struct fat_arch* archs = (struct fat_arch*)(p + sizeof(struct fat_header));
				for (unsigned long i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
					uint32_t fileOffset = OSSwapBigToHostInt32(archs[i].offset);
					try {
						switch ( OSSwapBigToHostInt32(archs[i].cputype) ) {
							case CPU_TYPE_POWERPC:
								fRebasers.push_back(new Rebaser<ppc>(&p[fileOffset]));
								break;
							case CPU_TYPE_I386:
								fRebasers.push_back(new Rebaser<x86>(&p[fileOffset]));
								break;
							case CPU_TYPE_X86_64:
								fRebasers.push_back(new Rebaser<x86_64>(&p[fileOffset]));
								break;
							case CPU_TYPE_ARM:
								fRebasers.push_back(new Rebaser<arm>(&p[fileOffset]));
								break;
							default:
								throw "unknown file format";
						}
					}
					catch (const char* msg) {
						fprintf(stderr, "rebase warning: %s for %s\n", msg, path);
					}
				}
			}
			else {
				try {
					if ( (OSSwapBigToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapBigToHostInt32(mh->cputype) == CPU_TYPE_POWERPC)) {
						fRebasers.push_back(new Rebaser<ppc>(mh));
					}
					else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_I386)) {
						fRebasers.push_back(new Rebaser<x86>(mh));
					}
					else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC_64) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_X86_64)) {
						fRebasers.push_back(new Rebaser<x86_64>(mh));
					}
					else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_ARM)) {
						fRebasers.push_back(new Rebaser<arm>(mh));
					}
					else {
						throw "unknown file format";
					}
				}
				catch (const char* msg) {
					fprintf(stderr, "rebase warning: %s for %s\n", msg, path);
				}
			}
			
			fMappingAddress = p;
			fFileSize = stat_buf.st_size;
		}


		~MultiArchRebaser()	{::munmap(fMappingAddress, fFileSize); }

	const std::vector<AbstractRebaser*>&		getArchs() const { return fRebasers; }
	void										commit()		{ ::msync(fMappingAddress, fFileSize, MS_ASYNC);  }

private:
	std::vector<AbstractRebaser*>				fRebasers;
	void*										fMappingAddress;
	uint64_t									fFileSize;
};
#endif


#endif // __MACHO_REBASER__




