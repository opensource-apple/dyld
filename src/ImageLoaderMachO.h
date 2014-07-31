/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
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


#ifndef __IMAGELOADERMACHO__
#define __IMAGELOADERMACHO__

#include <stdint.h> 

#include "ImageLoader.h"

struct sf_mapping;
struct _shared_region_mapping_np;

//
// ImageLoaderMachO is the concrete subclass of ImageLoader which loads mach-o format files.
// The class is written to be 64-bit clean and support both 32-bit and 64-bit executables in
// mach-o.
//
//
class ImageLoaderMachO : public ImageLoader {
public:
										ImageLoaderMachO(const char* path, int fd, const uint8_t firstPage[4096], uint64_t offset, uint64_t len, const struct stat& info, const LinkContext& context);
										ImageLoaderMachO(const char* moduleName, const struct mach_header* mh, uint64_t len, const LinkContext& context);
	virtual								~ImageLoaderMachO()  {}

	const char*							getInstallPath() const;
	virtual void*						getMain() const;
	virtual const struct mach_header*   machHeader() const;
	virtual uintptr_t					getSlide() const;
	virtual const void*					getBaseAddress() const;
	virtual bool						hasCoalescedExports() const;
	virtual const Symbol*				findExportedSymbol(const char* name, const void* hint, bool searchReExports, ImageLoader** foundIn) const;
	virtual uintptr_t					getExportedSymbolAddress(const Symbol* sym) const;
	virtual DefinitionFlags				getExportedSymbolInfo(const Symbol* sym) const;
	virtual const char*					getExportedSymbolName(const Symbol* sym) const;
	virtual uint32_t					getExportedSymbolCount() const;
	virtual const Symbol*				getIndexedExportedSymbol(uint32_t index) const;
	virtual uint32_t					getImportedSymbolCount() const;
	virtual const Symbol*				getIndexedImportedSymbol(uint32_t index) const;
	virtual ReferenceFlags				geImportedSymbolInfo(const Symbol* sym) const;
	virtual const char*					getImportedSymbolName(const Symbol* sym) const;
	virtual bool						isBundle() const;
	virtual bool						isDylib() const;
	virtual bool						forceFlat() const;
	virtual uintptr_t					doBindLazySymbol(uintptr_t* lazyPointer, const LinkContext& context);
	virtual void						doTermination(const LinkContext& context);
	virtual void						doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[]);
	virtual bool						needsInitialization();
	virtual bool						hasImageNotification();
	virtual bool						getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length);
	virtual bool						findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset);
	virtual bool						usablePrebinding(const LinkContext& context) const;
			
	
	static void							printStatistics(unsigned int imageCount);
	
protected:
						ImageLoaderMachO(const ImageLoaderMachO&);
	void				operator=(const ImageLoaderMachO&);

	virtual uint32_t	doGetDependentLibraryCount();
	virtual void		doGetDependentLibraries(DependentLibrary libs[]);
	virtual LibraryInfo doGetLibraryInfo();
	virtual void		doRebase(const LinkContext& context);
	virtual void		doBind(const LinkContext& context, BindingLaziness bindness);
	virtual void		doInitialization(const LinkContext& context);
	virtual void		doPrebinding(const LinkContext& context, time_t timestamp, uint8_t* fileToPrebind);
	virtual bool		needsTermination();
	virtual void		instantiateSegments(const uint8_t* fileData);
	virtual bool		segmentsMustSlideTogether() const;	
	virtual bool		segmentsCanSlide() const;			
	virtual void		setSlide(intptr_t slide);		
	virtual bool		usesTwoLevelNameSpace() const;
	virtual	bool		isSubframeworkOf(const LinkContext& context, const ImageLoader* image) const;
	virtual	bool		hasSubLibrary(const LinkContext& context, const ImageLoader* child) const;
	virtual bool		isPrebindable() const;
	virtual void		prebindUnmap(const LinkContext& context);

#if !__LP64__   // split segs not supported for 64-bits
	virtual void		mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
#endif 
		
private:
	friend class SegmentMachO;

			void		init();
			void		parseLoadCmds();
			void		doBindIndirectSymbolPointers(const LinkContext& context, BindingLaziness bindness, bool onlyCoalescedSymbols);
			void		doBindExternalRelocations(const LinkContext& context, bool onlyCoalescedSymbols);
			uintptr_t	resolveUndefined(const LinkContext& context, const struct macho_nlist* symbol, bool twoLevel, ImageLoader **foundIn);
			uintptr_t	getRelocBase();
			void		doImageInit(const LinkContext& context);
			void		doModInitFunctions(const LinkContext& context);
			void		setupLazyPointerHandler();
			void		applyPrebindingToDATA(uint8_t* fileToPrebind);
			void		applyPrebindingToLoadCommands(const LinkContext& context, uint8_t* fileToPrebind, time_t timestamp);
			void		applyPrebindingToLinkEdit(const LinkContext& context, uint8_t* fileToPrebind);
#if !__LP64__   // split segs not supported for 64-bits
			unsigned int getExtraZeroFillEntriesCount();
			int			sharedRegionLoadFile(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
			int			sharedRegionMapFile(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
			int			sharedRegionMakePrivate(const LinkContext& context);
			int			sharedRegionMapFilePrivate(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context, bool usemmap);
			int			sharedRegionMapFilePrivateOutside(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
			void		initMappingTable(uint64_t offsetInFat, sf_mapping *mappingTable, uintptr_t baseAddress);
			void		initMappingTable(uint64_t offsetInFat, _shared_region_mapping_np *mappingTable);
#endif
			bool		needsCoalescing() const;
			
	static	bool				symbolRequiresCoalescing(const struct macho_nlist* symbol); 
	static uintptr_t			bindLazySymbol(const mach_header*, uintptr_t* lazyPointer);
	static const struct macho_nlist*  binarySearch(const char* key, const char stringPool[], const struct macho_nlist symbols[], uint32_t symbolCount);
	static const struct macho_nlist*  binarySearchWithToc(const char* key, const char stringPool[], const struct macho_nlist symbols[], 
												const struct dylib_table_of_contents toc[], uint32_t symbolCount, uint32_t hintIndex);
			
	const uint8_t*							fMachOData;
	const uint8_t*							fLinkEditBase; // add any internal "offset" to this to get actual address
	const struct macho_nlist*				fSymbolTable;
	const char*								fStrings;
	const struct dysymtab_command*			fDynamicInfo;
	uintptr_t								fSlide;
	bool									fIsSplitSeg;
	bool									fHasSubLibraries;
	bool									fHasSubUmbrella;
	bool									fTextSegmentHasFixups;
	const struct macho_routines_command*	fDashInit;
	const struct macho_section*				fModInitSection;
	const struct macho_section*				fModTermSection;
	const struct macho_section*				fDATAdyld;
	const struct macho_section*				fImageNotifySection;
	const struct twolevel_hints_command*	fTwoLevelHints;
	const struct dylib_command*				fDylibID;
	const char*								fReExportThruFramework;
	SegmentMachO*							fTextSegmentWithFixups; // NULL unless __TEXT segment has fixups

	static uint32_t					fgHintedBinaryTreeSearchs;
	static uint32_t					fgUnhintedBinaryTreeSearchs;
};


class SegmentMachO : public Segment
{
public:	
								SegmentMachO(const struct macho_segment_command* cmd, ImageLoaderMachO*, const uint8_t* fileData);
	virtual						~SegmentMachO();
								
	virtual const ImageLoader*	getImage();
	virtual const char*			getName();
	virtual uintptr_t			getSize();
	virtual uintptr_t			getFileSize();
	virtual uintptr_t			getFileOffset();
	virtual bool				readable();
	virtual bool				writeable();
	virtual bool				executable();
	virtual bool				unaccessible();
	virtual bool				hasFixUps();
	virtual uintptr_t			getActualLoadAddress();
	virtual uintptr_t			getPreferredLoadAddress();
	virtual void				setUnMapWhenDestructed(bool unmap);
	virtual uint32_t			crc32();

protected:
	virtual bool				hasPreferredLoadAddress();
	
private:
						SegmentMachO(const SegmentMachO&);
	void				operator=(const SegmentMachO&);

	friend class ImageLoaderMachO;
	

	ImageLoaderMachO* const fImage;
	char					fName[18];
	const uintptr_t			fSize;
	const uintptr_t			fFileSize;
	const uintptr_t			fFileOffset;
	const uintptr_t			fPreferredLoadAddress;
	const uint16_t			fVMProtection;
	bool					fHasFixUps;
	bool					fUnMapOnDestruction;
};


#endif // __IMAGELOADERMACHO__




