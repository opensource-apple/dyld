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

#ifndef __MACHO_LAYOUT__
#define __MACHO_LAYOUT__

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

#include <vector>
#include <set>
#include <ext/hash_map>

#include "MachOFileAbstraction.hpp"
#include "Architectures.hpp"


void throwf(const char* format, ...) __attribute__((format(printf, 1, 2)));

__attribute__((noreturn))
void throwf(const char* format, ...) 
{
	va_list	list;
	char*	p;
	va_start(list, format);
	vasprintf(&p, format, list);
	va_end(list);
	
	const char*	t = p;
	throw t;
}


class MachOLayoutAbstraction
{
public:
	struct Segment
	{
	public:
					Segment(uint64_t addr, uint64_t vmsize, uint64_t offset, uint64_t file_size, 
							uint32_t prot, const char* segName) : fAddress(addr), fSize(vmsize),
							fFileOffset(offset), fFileSize(file_size), fPermissions(prot),
							fNewAddress(0), fMappedAddress(NULL) {
								strlcpy(fName, segName, 16);
							}
							
		uint64_t	address() const		{ return fAddress; }
		uint64_t	size() const		{ return fSize; }
		uint64_t	fileOffset() const	{ return fFileOffset; }
		uint64_t	fileSize() const	{ return fFileSize; }
		uint32_t	permissions() const { return fPermissions; }
		bool		readable() const	{ return fPermissions & VM_PROT_READ; }
		bool		writable() const	{ return fPermissions & VM_PROT_WRITE; }
		bool		executable() const	{ return fPermissions & VM_PROT_EXECUTE; }
		const char* name() const		{ return fName; }
		uint64_t	newAddress() const	{ return fNewAddress; }
		void*		mappedAddress() const			{ return fMappedAddress; }
		void		setNewAddress(uint64_t addr)	{ fNewAddress = addr; }
		void		setMappedAddress(void* addr)	{ fMappedAddress = addr; }
		void		setSize(uint64_t new_size)		{ fSize = new_size; }
		void		setFileOffset(uint64_t new_off)	{ fFileOffset = new_off; }
		void		setFileSize(uint64_t new_size)	{ fFileSize = new_size; }
		void		setWritable(bool w)		{ if (w) fPermissions |= VM_PROT_WRITE; else fPermissions &= ~VM_PROT_WRITE; }
	private:
		uint64_t	fAddress;
		uint64_t	fSize;
		uint64_t	fFileOffset;
		uint64_t	fFileSize;
		uint64_t	fNewAddress;
		void*		fMappedAddress;
		uint32_t	fPermissions;
		char		fName[16];
	};

	struct Library
	{
		const char*	name;
		uint32_t	currentVersion;
		uint32_t	compatibilityVersion;
	};
	
	
	virtual cpu_type_t							getArchitecture() const = 0;
	virtual const char*							getFilePath() const = 0;
	virtual uint64_t							getOffsetInUniversalFile() const	= 0;
	virtual uint32_t							getFileType() const	= 0;
	virtual uint32_t							getFlags() const = 0;
	virtual	Library								getID() const = 0;
	virtual bool								isSplitSeg() const = 0;
	virtual bool								hasSplitSegInfo() const = 0;
	virtual	uint32_t							getNameFileOffset() const = 0;
	virtual time_t								getLastModTime() const = 0;
	virtual ino_t								getInode() const = 0;
	virtual std::vector<Segment>&				getSegments() = 0;
	virtual const std::vector<Segment>&			getSegments() const = 0;
	virtual const std::vector<Library>&			getLibraries() const = 0;
	virtual uint64_t							getBaseAddress() const = 0;
	virtual uint64_t							getVMSize() const = 0;
	virtual uint64_t							getBaseExecutableAddress() const = 0;
	virtual uint64_t							getBaseWritableAddress() const = 0;
	virtual uint64_t							getBaseReadOnlyAddress() const = 0;
	virtual uint64_t							getExecutableVMSize() const = 0;
	virtual uint64_t							getWritableVMSize() const = 0;
	virtual uint64_t							getReadOnlyVMSize() const = 0;
};




template <typename A>
class MachOLayout : public MachOLayoutAbstraction
{
public:
												MachOLayout(const void* machHeader, uint64_t offset, const char* path, ino_t inode, time_t modTime);
	virtual										~MachOLayout() {}

	virtual cpu_type_t							getArchitecture() const;
	virtual const char*							getFilePath() const		{ return fPath; }
	virtual uint64_t							getOffsetInUniversalFile() const { return fOffset; }
	virtual uint32_t							getFileType() const		{ return fFileType; }
	virtual uint32_t							getFlags() const		{ return fFlags; }
	virtual	Library								getID() const			{ return fDylibID; }
	virtual bool								isSplitSeg() const;
	virtual bool								hasSplitSegInfo() const	{ return fHasSplitSegInfo; }
	virtual	uint32_t							getNameFileOffset() const{ return fNameFileOffset; }
	virtual time_t								getLastModTime() const	{ return fMTime; }
	virtual ino_t								getInode() const		{ return fInode; }
	virtual std::vector<Segment>&				getSegments()			{ return fSegments; }
	virtual const std::vector<Segment>&			getSegments() const		{ return fSegments; }
	virtual const std::vector<Library>&			getLibraries() const	{ return fLibraries; }
	virtual uint64_t							getBaseAddress() const	{ return fLowSegment->address(); }
	virtual uint64_t							getVMSize() const		{ return fVMSize; }
	virtual uint64_t							getBaseExecutableAddress() const { return fLowExecutableSegment->address(); }
	virtual uint64_t							getBaseWritableAddress() const	{ return fLowWritableSegment->address(); }
	virtual uint64_t							getBaseReadOnlyAddress() const	{ return fLowReadOnlySegment->address(); }
	virtual uint64_t							getExecutableVMSize() const		{ return fVMExecutableSize; }
	virtual uint64_t							getWritableVMSize() const		{ return fVMWritablSize; }
	virtual uint64_t							getReadOnlyVMSize() const		{ return fVMReadOnlySize; }

private:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
	
	const char*									fPath;
	uint64_t									fOffset;
	uint32_t									fFileType;
	uint32_t									fFlags;
	std::vector<Segment>						fSegments;
	std::vector<Library>						fLibraries;
	const Segment*								fLowSegment;
	const Segment*								fLowExecutableSegment;
	const Segment*								fLowWritableSegment;
	const Segment*								fLowReadOnlySegment;
	Library										fDylibID;
	uint32_t									fNameFileOffset;
	time_t										fMTime;
	ino_t										fInode;
	uint64_t									fVMSize;
	uint64_t									fVMExecutableSize;
	uint64_t									fVMWritablSize;
	uint64_t									fVMReadOnlySize;
	bool										fHasSplitSegInfo;
};



class UniversalMachOLayout
{
public:
												UniversalMachOLayout(const char* path, const std::set<cpu_type_t>* onlyArchs=NULL);
												~UniversalMachOLayout() {}

	static const UniversalMachOLayout*			find(const char* path, const std::set<cpu_type_t>* onlyArchs=NULL);
	const MachOLayoutAbstraction*				getArch(cpu_type_t) const;
	const std::vector<MachOLayoutAbstraction*>&	getArchs() const { return fLayouts; }

private:
	struct CStringEquals {
		bool operator()(const char* left, const char* right) const { return (strcmp(left, right) == 0); }
	};
	typedef __gnu_cxx::hash_map<const char*, const UniversalMachOLayout*, __gnu_cxx::hash<const char*>, CStringEquals> PathToNode;

	static PathToNode							fgLayoutCache;
	const char*									fPath;
	std::vector<MachOLayoutAbstraction*>		fLayouts;
};

UniversalMachOLayout::PathToNode UniversalMachOLayout::fgLayoutCache;


const MachOLayoutAbstraction* UniversalMachOLayout::getArch(cpu_type_t arch) const
{
	for(std::vector<MachOLayoutAbstraction*>::const_iterator it=fLayouts.begin(); it != fLayouts.end(); ++it) {
		const MachOLayoutAbstraction* layout = *it;
		if ( layout->getArchitecture() == arch ) 
			return layout;
	}
	return NULL;
}
 

const UniversalMachOLayout* UniversalMachOLayout::find(const char* path, const std::set<cpu_type_t>* onlyArchs)
{
	// look in cache
	PathToNode::iterator pos = fgLayoutCache.find(path);
	if ( pos != fgLayoutCache.end() )
		return pos->second;
		
	// create UniversalMachOLayout
	const UniversalMachOLayout* result = new UniversalMachOLayout(path, onlyArchs);
	
	// add it to cache
	fgLayoutCache[result->fPath] = result;
	
	return result;
}


UniversalMachOLayout::UniversalMachOLayout(const char* path, const std::set<cpu_type_t>* onlyArchs)
 : fPath(strdup(path))
{
	// map in whole file
	int fd = ::open(path, O_RDONLY, 0);
	if ( fd == -1 )
		throwf("can't open file, errno=%d", errno);
	struct stat stat_buf;
	if ( fstat(fd, &stat_buf) == -1)
		throwf("can't stat open file %s, errno=%d", path, errno);
	if ( stat_buf.st_size < 20 )
		throwf("file too small %s", path);
	uint8_t* p = (uint8_t*)::mmap(NULL, stat_buf.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
	if ( p == (uint8_t*)(-1) )
		throwf("can't map file %s, errno=%d", path, errno);
	::close(fd);

	try {
		// if fat file, process each architecture
		const fat_header* fh = (fat_header*)p;
		const mach_header* mh = (mach_header*)p;
		if ( fh->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
			// Fat header is always big-endian
			const struct fat_arch* archs = (struct fat_arch*)(p + sizeof(struct fat_header));
			for (unsigned long i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
				uint32_t fileOffset = OSSwapBigToHostInt32(archs[i].offset);
				cpu_type_t curArch =  OSSwapBigToHostInt32(archs[i].cputype);
				if ( fileOffset > stat_buf.st_size )
					throwf("malformed universal file, slice for architecture 0x%08X is beyond end of file: %s", curArch, path);
				try {
					if ( (onlyArchs == NULL) || (onlyArchs->count(curArch) != 0) ) {
						switch ( curArch ) {
							case CPU_TYPE_POWERPC:
								fLayouts.push_back(new MachOLayout<ppc>(&p[fileOffset], fileOffset, fPath, stat_buf.st_ino, stat_buf.st_mtime));
								break;
							case CPU_TYPE_POWERPC64:
								fLayouts.push_back(new MachOLayout<ppc64>(&p[fileOffset], fileOffset, fPath, stat_buf.st_ino, stat_buf.st_mtime));
								break;
							case CPU_TYPE_I386:
								fLayouts.push_back(new MachOLayout<x86>(&p[fileOffset], fileOffset, fPath, stat_buf.st_ino, stat_buf.st_mtime));
								break;
							case CPU_TYPE_X86_64:
								fLayouts.push_back(new MachOLayout<x86_64>(&p[fileOffset], fileOffset, fPath, stat_buf.st_ino, stat_buf.st_mtime));
								break;
							default:
								throw "unknown file format";
						}
					}
				}
				catch (const char* msg) {
					fprintf(stderr, "warning: %s for %s\n", msg, path);
				}
			}
		}
		else {
			try {
				if ( (OSSwapBigToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapBigToHostInt32(mh->cputype) == CPU_TYPE_POWERPC)) {
					if ( (onlyArchs == NULL) || (onlyArchs->count(CPU_TYPE_POWERPC) != 0) ) 
						fLayouts.push_back(new MachOLayout<ppc>(mh, 0, fPath, stat_buf.st_ino, stat_buf.st_mtime));
				}
				else if ( (OSSwapBigToHostInt32(mh->magic) == MH_MAGIC_64) && (OSSwapBigToHostInt32(mh->cputype) == CPU_TYPE_POWERPC64)) {
					if ( (onlyArchs == NULL) || (onlyArchs->count(CPU_TYPE_POWERPC64) != 0) ) 
						fLayouts.push_back(new MachOLayout<ppc64>(mh, 0, fPath, stat_buf.st_ino, stat_buf.st_mtime));
				}
				else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_I386)) {
					if ( (onlyArchs == NULL) || (onlyArchs->count(CPU_TYPE_I386) != 0) ) 
						fLayouts.push_back(new MachOLayout<x86>(mh, 0, fPath, stat_buf.st_ino, stat_buf.st_mtime));
				}
				else if ( (OSSwapLittleToHostInt32(mh->magic) == MH_MAGIC_64) && (OSSwapLittleToHostInt32(mh->cputype) == CPU_TYPE_X86_64)) {
					if ( (onlyArchs == NULL) || (onlyArchs->count(CPU_TYPE_X86_64) != 0) ) 
						fLayouts.push_back(new MachOLayout<x86_64>(mh, 0, fPath, stat_buf.st_ino, stat_buf.st_mtime));
				}
				else {
					throw "unknown file format";
				}
			}
			catch (const char* msg) {
				fprintf(stderr, "warning: %s for %s\n", msg, path);
			}
		}
	}
	catch (...) {
		::munmap(p, stat_buf.st_size);
		throw;
	}
}



template <typename A>
MachOLayout<A>::MachOLayout(const void* machHeader, uint64_t offset, const char* path, ino_t inode, time_t modTime)
 : fPath(path), fOffset(offset), fMTime(modTime), fInode(inode), fHasSplitSegInfo(false)
{
	fDylibID.name = NULL;
	fDylibID.currentVersion = 0;
	fDylibID.compatibilityVersion = 0;
	
	const macho_header<P>* mh = (const macho_header<P>*)machHeader;
	if ( mh->cputype() != getArchitecture() )
		throw "wrong architecture";
	switch ( mh->filetype() ) {
		case MH_DYLIB:
		case MH_BUNDLE:
		case MH_EXECUTE:
		case MH_DYLIB_STUB:
		case MH_DYLINKER:
			break;
		default:
			throw "file is not a mach-o final linked image";
	}
	fFlags = mh->flags();
	fFileType = mh->filetype();
	
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)mh + sizeof(macho_header<P>));
	const uint32_t cmd_count = mh->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch ( cmd->cmd() ) {
			case LC_ID_DYLIB:	
				{
					macho_dylib_command<P>* dylib  = (macho_dylib_command<P>*)cmd;
					fDylibID.name = strdup(dylib->name());
					fDylibID.currentVersion = dylib->current_version();
					fDylibID.compatibilityVersion = dylib->compatibility_version();
					fNameFileOffset = dylib->name() - (char*)machHeader;
				}
				break;
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
				{
					macho_dylib_command<P>* dylib = (macho_dylib_command<P>*)cmd;
					Library lib;
					lib.name = strdup(dylib->name());
					lib.currentVersion = dylib->current_version();
					lib.compatibilityVersion = dylib->compatibility_version();
					fLibraries.push_back(lib);
				}
				break;
			case LC_SEGMENT_SPLIT_INFO:
				fHasSplitSegInfo = true;
				break;
			case macho_segment_command<P>::CMD:
				{
					macho_segment_command<P>* segCmd = (macho_segment_command<P>*)cmd;
					fSegments.push_back(Segment(segCmd->vmaddr(), segCmd->vmsize(), segCmd->fileoff(), 
								segCmd->filesize(), segCmd->initprot(), segCmd->segname()));
				}
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}

	fLowSegment = NULL;
	fLowExecutableSegment = NULL;
	fLowWritableSegment = NULL;
	fLowReadOnlySegment = NULL;
	fVMExecutableSize = 0;
	fVMWritablSize = 0;
	fVMReadOnlySize = 0;
	fVMSize = 0;
	const Segment* highSegment = NULL;
	for(std::vector<Segment>::const_iterator it = fSegments.begin(); it != fSegments.end(); ++it) {
		const Segment& seg = *it;
		if ( (fLowSegment == NULL) || (seg.address() < fLowSegment->address()) )
			fLowSegment = &seg;
		if ( (highSegment == NULL) || (seg.address() > highSegment->address()) )
			highSegment = &seg;
		if ( seg.executable() ) {
			if ( (fLowExecutableSegment == NULL) || (seg.address() < fLowExecutableSegment->address()) )
				fLowExecutableSegment = &seg;
			fVMExecutableSize += seg.size();
		}
		else if ( seg.writable()) {
			if ( (fLowWritableSegment == NULL) || (seg.address() < fLowWritableSegment->address()) )
				fLowWritableSegment = &seg;
			fVMWritablSize += seg.size();
		}
		else {
			if ( (fLowReadOnlySegment == NULL) || (seg.address() < fLowReadOnlySegment->address()) )
				fLowReadOnlySegment = &seg;
			fVMReadOnlySize += seg.size();
		}
	}
	if ( (highSegment != NULL) && (fLowSegment != NULL) )
		fVMSize = (highSegment->address() + highSegment->size() - fLowSegment->address() + 4095) & (-4096);			
}

template <> cpu_type_t MachOLayout<ppc>::getArchitecture()    const { return CPU_TYPE_POWERPC; }
template <> cpu_type_t MachOLayout<ppc64>::getArchitecture()  const { return CPU_TYPE_POWERPC64; }
template <> cpu_type_t MachOLayout<x86>::getArchitecture()    const { return CPU_TYPE_I386; }
template <> cpu_type_t MachOLayout<x86_64>::getArchitecture() const { return CPU_TYPE_X86_64; }

template <>
bool MachOLayout<ppc>::isSplitSeg() const
{
	return ( (this->getFlags() & MH_SPLIT_SEGS) != 0 );
}

template <>
bool MachOLayout<x86>::isSplitSeg() const
{
	return ( (this->getFlags() & MH_SPLIT_SEGS) != 0 );
}

template <typename A>
bool MachOLayout<A>::isSplitSeg() const
{
	return false;
}


#endif // __MACHO_LAYOUT__



