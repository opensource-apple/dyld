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
#include <vector>
#include <set>

#include "MachOFileAbstraction.hpp"
#include "Architectures.hpp"
#include "MachOLayout.hpp"



class AbstractRebaser
{
public:
	virtual cpu_type_t							getArchitecture() const = 0;
	virtual uint64_t							getBaseAddress() const = 0;
	virtual uint64_t							getVMSize() const = 0;
	virtual void								rebase() = 0;
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
	virtual void								rebase();

protected:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
		
	pint_t*										mappedAddressForNewAddress(pint_t vmaddress);
	pint_t										getSlideForNewAddress(pint_t newAddress);
	
private:
	pint_t										calculateRelocBase();
	void										adjustLoadCommands();
	void										adjustSymbolTable();
	void										adjustDATA();
	void										adjustCode();
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
	bool										fSplittingSegments;
};



template <typename A>
Rebaser<A>::Rebaser(const MachOLayoutAbstraction& layout)
 : 	fLayout(layout), fOrignalVMRelocBaseAddress(NULL), fLinkEditBase(NULL), fSplittingSegments(false)
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
		
	fOrignalVMRelocBaseAddress = calculateRelocBase();
	
	fSplittingSegments = layout.hasSplitSegInfo() && this->unequalSlides();
}

template <> cpu_type_t Rebaser<ppc>::getArchitecture()    const { return CPU_TYPE_POWERPC; }
template <> cpu_type_t Rebaser<ppc64>::getArchitecture()  const { return CPU_TYPE_POWERPC64; }
template <> cpu_type_t Rebaser<x86>::getArchitecture()    const { return CPU_TYPE_I386; }
template <> cpu_type_t Rebaser<x86_64>::getArchitecture() const { return CPU_TYPE_X86_64; }

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
void Rebaser<A>::rebase()
{		
	// update writable segments that have internal pointers
	this->adjustDATA();

	// if splitting segments, update code-to-data references
	this->adjustCode();
	
	// change address on relocs now that segments are split
	this->adjustRelocBaseAddresses();
	
	// update load commands
	this->adjustLoadCommands();
	
	// update symbol table  
	this->adjustSymbolTable();
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
	return this->mappedAddressForVMAddress(r_address + fOrignalVMRelocBaseAddress);
}


template <typename A>
void Rebaser<A>::adjustSymbolTable()
{
	const macho_dysymtab_command<P>* dysymtab = NULL;
	macho_nlist<P>* symbolTable = NULL;

	// get symbol table info
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_SYMTAB:
				{
					const macho_symtab_command<P>* symtab = (macho_symtab_command<P>*)cmd;
					symbolTable = (macho_nlist<P>*)(&fLinkEditBase[symtab->symoff()]);
				}
				break;
			case LC_DYSYMTAB:
				dysymtab = (macho_dysymtab_command<P>*)cmd;
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	

	// walk all exports and slide their n_value
	macho_nlist<P>* lastExport = &symbolTable[dysymtab->iextdefsym()+dysymtab->nextdefsym()];
	for (macho_nlist<P>* entry = &symbolTable[dysymtab->iextdefsym()]; entry < lastExport; ++entry) {
		if ( (entry->n_type() & N_TYPE) == N_SECT )
			entry->set_n_value(entry->n_value() + this->getSlideForVMAddress(entry->n_value()));
	}

	// walk all local symbols and slide their n_value (don't adjust and stabs)
	macho_nlist<P>*  lastLocal = &symbolTable[dysymtab->ilocalsym()+dysymtab->nlocalsym()];
	for (macho_nlist<P>* entry = &symbolTable[dysymtab->ilocalsym()]; entry < lastLocal; ++entry) {
		if ( (entry->n_sect() != NO_SECT) && ((entry->n_type() & N_STAB) == 0) )
			entry->set_n_value(entry->n_value() + this->getSlideForVMAddress(entry->n_value()));
	}
}




template <typename A>
void Rebaser<A>::doCodeUpdate(uint8_t kind, uint64_t address, int64_t codeToDataDelta, int64_t codeToImportDelta)
{
	//fprintf(stderr, "doCodeUpdate(kind=%d, address=0x%0llX, dataDelta=0x%08llX, importDelta=0x%08llX)\n", kind, address, codeToDataDelta, codeToImportDelta);
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
		case 3: // used only for ppc/ppc64, an instruction that sets the hi16 of a register
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
		for(const uint8_t* p = infoStart; *p != 0;) {
			uint8_t kind = *p++;
			p = this->doCodeUpdateForEachULEB128Address(p, kind, orgBaseAddress, codeToDataDelta, codeToImportDelta);
		}
	}
}


template <typename A>
void Rebaser<A>::adjustDATA()
{
	const macho_dysymtab_command<P>* dysymtab = NULL;

	// get symbol table info
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_DYSYMTAB:
				dysymtab = (macho_dysymtab_command<P>*)cmd;
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	


	// walk all local relocations and slide every pointer
	const macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[dysymtab->locreloff()]);
	const macho_relocation_info<P>* const relocsEnd = &relocsStart[dysymtab->nlocrel()];
	for (const macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		this->doLocalRelocation(reloc);
	}
	
	// walk non-lazy-pointers and slide the ones that are LOCAL
	cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			const macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			const macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
			const macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
			const uint32_t* const indirectTable = (uint32_t*)(&fLinkEditBase[dysymtab->indirectsymoff()]);
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
	// split seg file alreday have reloc base of first writable segment
	// only non-split-segs that are being split need this adjusted
	if ( (fHeader->flags() & MH_SPLIT_SEGS) == 0  ) {

		// get symbol table to find relocation records
		const macho_dysymtab_command<P>* dysymtab = NULL;
		const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
		const uint32_t cmd_count = fHeader->ncmds();
		const macho_load_command<P>* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd()) {
				case LC_DYSYMTAB:
					dysymtab = (macho_dysymtab_command<P>*)cmd;
					break;
			}
			cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
		}	

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
		macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[dysymtab->locreloff()]);
		macho_relocation_info<P>* const relocsEnd = &relocsStart[dysymtab->nlocrel()];
		for (macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
			reloc->set_r_address(reloc->r_address()-relocAddressAdjust);
		}
		
		// walk all external relocations and adjust every address 
		macho_relocation_info<P>* const externRelocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[dysymtab->extreloff()]);
		macho_relocation_info<P>* const externRelocsEnd = &externRelocsStart[dysymtab->nextrel()];
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
typename A::P::uint_t Rebaser<A>::calculateRelocBase()
{
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	if ( fHeader->flags() & MH_SPLIT_SEGS ) {
		// reloc addresses are from the start of the first writable segment
		for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
			const MachOLayoutAbstraction::Segment& seg = *it;
			if ( seg.writable() ) {
				// found first writable segment
				return seg.address();
			}
		}
		throw "no writable segment";
	}
	else {
		// reloc addresses are from the start of the mapped file (base address)
		return segments[0].address();
	}
}

template <>
ppc64::P::uint_t Rebaser<ppc64>::calculateRelocBase()
{
	// reloc addresses either:
	// 1) from the first segment vmaddr if no writable segment is > 4GB from first segment vmaddr
	// 2) from start of first writable segment
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	uint64_t threshold = segments[0].address() + 0x100000000ULL;
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( seg.writable() && (seg.address()+seg.size()) > threshold ) {
			// found writable segment with address > 4GB past base address
			return seg.address();
		}
	}
	// just use base address
	return segments[0].address();
}

template <>
x86_64::P::uint_t Rebaser<x86_64>::calculateRelocBase()
{
	// reloc addresses are always based from the start of the first writable segment
	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( seg.writable() ) {
			// found first writable segment
			return seg.address();
		}
	}
	throw "no writable segment";
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
							case CPU_TYPE_POWERPC64:
								fRebasers.push_back(new Rebaser<ppc64>(&p[fileOffset]));
								break;
							case CPU_TYPE_I386:
								fRebasers.push_back(new Rebaser<x86>(&p[fileOffset]));
								break;
							case CPU_TYPE_X86_64:
								fRebasers.push_back(new Rebaser<x86_64>(&p[fileOffset]));
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
					else if ( (OSSwapBigToHostInt32(mh->magic) == MH_MAGIC_64) && (OSSwapBigToHostInt32(mh->cputype) == CPU_TYPE_POWERPC64)) {
						fRebasers.push_back(new Rebaser<ppc64>(mh));
					}
					else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_I386)) {
						fRebasers.push_back(new Rebaser<x86>(mh));
					}
					else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC_64) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_X86_64)) {
						fRebasers.push_back(new Rebaser<x86_64>(mh));
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




