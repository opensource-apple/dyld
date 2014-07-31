/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2009 Apple Inc. All rights reserved.
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
#include <string.h>
#include <malloc/malloc.h>

#include <crt_externs.h>
#include <Availability.h>

#include "mach-o/dyld.h"
#include "mach-o/dyld_priv.h"

#include "dyldLock.h"

extern "C" int  __cxa_atexit(void (*func)(void *), void *arg, void *dso);
extern "C" void __cxa_finalize(const void *dso);

#ifndef LC_LOAD_UPWARD_DYLIB
	#define	LC_LOAD_UPWARD_DYLIB (0x23|LC_REQ_DYLD)	/* load of dylib whose initializers run later */
#endif

#define DYLD_SHARED_CACHE_SUPPORT (__ppc__ || __i386__ || __ppc64__ || __x86_64__ || __arm__)

// deprecated APIs are still availble on Mac OS X, but not on iPhone OS
#if __IPHONE_OS_VERSION_MIN_REQUIRED	
	#define DEPRECATED_APIS_SUPPORTED 0
#else
	#define DEPRECATED_APIS_SUPPORTED 1
#endif

/*
 * names_match() takes an install_name from an LC_LOAD_DYLIB command and a
 * libraryName (which is -lx or -framework Foo argument passed to the static
 * link editor for the same library) and determines if they match.  This depends
 * on conventional use of names including major versioning.
 */
static
bool
names_match(
char *install_name,
const char* libraryName)
{
    char *basename;
    unsigned long n;

	/*
	 * Conventional install names have these forms:
	 *	/System/Library/Frameworks/AppKit.framework/Versions/A/Appkit
	 *	/Local/Library/Frameworks/AppKit.framework/Appkit
	 *	/lib/libsys_s.A.dylib
	 *	/usr/lib/libsys_s.dylib
	 */
	basename = strrchr(install_name, '/');
	if(basename == NULL)
	    basename = install_name;
	else
	    basename++;

	/*
	 * By checking the base name matching the library name we take care
	 * of the -framework cases.
	 */
	if(strcmp(basename, libraryName) == 0)
	    return(TRUE);

	/*
	 * Now check the base name for "lib" if so proceed to check for the
	 * -lx case dealing with a possible .X.dylib and a .dylib extension.
	 */
	if(strncmp(basename, "lib", 3) ==0){
	    n = strlen(libraryName);
	    if(strncmp(basename+3, libraryName, n) == 0){
		if(strncmp(basename+3+n, ".dylib", 6) == 0)
		    return(TRUE);
		if(basename[3+n] == '.' &&
		   basename[3+n+1] != '\0' &&
		   strncmp(basename+3+n+2, ".dylib", 6) == 0)
		    return(TRUE);
	    }
	}
	return(FALSE);
}

#if DEPRECATED_APIS_SUPPORTED

void NSInstallLinkEditErrorHandlers(
const NSLinkEditErrorHandlers* handlers)
{
	DYLD_LOCK_THIS_BLOCK;
	typedef void (*ucallback_t)(const char* symbol_name);
 	typedef NSModule (*mcallback_t)(NSSymbol s, NSModule old, NSModule newhandler);
	typedef void (*lcallback_t)(NSLinkEditErrors c, int errorNumber,
								const char* fileName, const char* errorString);
	static void (*p)(ucallback_t undefined, mcallback_t multiple, lcallback_t linkEdit) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_install_handlers", (void**)&p);
	mcallback_t m = handlers->multiple;
	p(handlers->undefined, m, handlers->linkEdit);
}

const char* 
NSNameOfModule(
NSModule module)
{
	DYLD_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSModule module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSNameOfModule", (void**)&p);
	return(p(module));
} 

const char* 
NSLibraryNameForModule(
NSModule module)
{
	DYLD_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSModule module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLibraryNameForModule", (void**)&p);
	return(p(module));
}

bool
NSIsSymbolNameDefined(
const char* symbolName)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefined", (void**)&p);
	return(p(symbolName));
}

bool
NSIsSymbolNameDefinedWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const char* symbolName,
			  const char* libraryNameHint) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefinedWithHint", (void**)&p);
	return(p(symbolName, libraryNameHint));
}

bool
NSIsSymbolNameDefinedInImage(
const struct mach_header *image,
const char* symbolName)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const struct mach_header *image,
			  const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefinedInImage", (void**)&p);
	return(p(image, symbolName));
}

NSSymbol
NSLookupAndBindSymbol(
const char* symbolName)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupAndBindSymbol", (void**)&p);
	return(p(symbolName));
}

NSSymbol
NSLookupAndBindSymbolWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(const char* symbolName,
			 const char* libraryNameHint) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupAndBindSymbolWithHint", (void**)&p);
	return(p(symbolName, libraryNameHint));
}

NSSymbol
NSLookupSymbolInModule(
NSModule module,
const char* symbolName)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(NSModule module, const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupSymbolInModule", (void**)&p);
	return(p(module, symbolName));
}

NSSymbol
NSLookupSymbolInImage(
const struct mach_header *image,
const char* symbolName,
uint32_t options)
{
 	DYLD_LOCK_THIS_BLOCK;
   static NSSymbol (*p)(const struct mach_header *image,
			 const char* symbolName,
			 uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupSymbolInImage", (void**)&p);
	return(p(image, symbolName, options));
}

const char* 
NSNameOfSymbol(
NSSymbol symbol)
{
	DYLD_LOCK_THIS_BLOCK;
    static char * (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSNameOfSymbol",(void**)&p);
	return(p(symbol));
}

void *
NSAddressOfSymbol(
NSSymbol symbol)
{
	DYLD_LOCK_THIS_BLOCK;
    static void * (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddressOfSymbol", (void**)&p);
	return(p(symbol));
}

NSModule
NSModuleForSymbol(
NSSymbol symbol)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSModule (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSModuleForSymbol", (void**)&p);
	return(p(symbol));
}

bool
NSAddLibrary(
const char* pathName)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const char* pathName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddLibrary", (void**)&p);
	return(p(pathName));
}

bool
NSAddLibraryWithSearching(
const char* pathName)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const char* pathName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddLibraryWithSearching", (void**)&p);
	return(p(pathName));
}

const struct mach_header *
NSAddImage(
const char* image_name,
uint32_t options)
{
	DYLD_LOCK_THIS_BLOCK;
    static const struct mach_header * (*p)(const char* image_name,
					   uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddImage", (void**)&p);
	return(p(image_name, options));
}
#endif // DEPRECATED_APIS_SUPPORTED

/*
 * This routine returns the current version of the named shared library the
 * executable it was built with.  The libraryName parameter is the same as the
 * -lx or -framework Foo argument passed to the static link editor when building
 * the executable (with -lx it would be "x" and with -framework Foo it would be
 * "Foo").  If this the executable was not built against the specified library
 * it returns -1.  It should be noted that if this only returns the value the
 * current version of the named shared library the executable was built with
 * and not a list of current versions that dependent libraries and bundles the
 * program is using were built with.
 */
int32_t
NSVersionOfLinkTimeLibrary(
const char* libraryName)
{
    unsigned long i;
    struct load_command *load_commands, *lc;
    struct dylib_command *dl;
    char *install_name;
#if __LP64__
    static struct mach_header_64 *mh = NULL;
#else
    static struct mach_header *mh = NULL;
#endif
	if(mh == NULL)
	    mh = _NSGetMachExecuteHeader();
	load_commands = (struct load_command *)
#if __LP64__
			    ((char *)mh + sizeof(struct mach_header_64));
#else
			    ((char *)mh + sizeof(struct mach_header));
#endif
	lc = load_commands;
	for(i = 0; i < mh->ncmds; i++){
		switch ( lc->cmd ) { 
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				dl = (struct dylib_command *)lc;
				install_name = (char *)dl + dl->dylib.name.offset;
				if(names_match(install_name, libraryName) == TRUE)
					return(dl->dylib.current_version);
				break;
		}
	    lc = (struct load_command *)((char *)lc + lc->cmdsize);
	}
	return(-1);
}

/*
 * This routine returns the current version of the named shared library the
 * program it is running against.  The libraryName parameter is the same as
 * would be static link editor using the -lx or -framework Foo flags (with -lx
 * it would be "x" and with -framework Foo it would be "Foo").  If the program
 * is not using the specified library it returns -1.
 */
int32_t
NSVersionOfRunTimeLibrary(
const char* libraryName)
{
    unsigned long i, j, n;
    char *install_name;
    struct load_command *load_commands, *lc;
    struct dylib_command *dl;
    const struct mach_header *mh;

	n = _dyld_image_count();
	for(i = 0; i < n; i++){
	    mh = _dyld_get_image_header(i);
	    if(mh->filetype != MH_DYLIB)
		continue;
	    load_commands = (struct load_command *)
#if __LP64__
			    ((char *)mh + sizeof(struct mach_header_64));
#else
			    ((char *)mh + sizeof(struct mach_header));
#endif
	    lc = load_commands;
	    for(j = 0; j < mh->ncmds; j++){
		if(lc->cmd == LC_ID_DYLIB){
		    dl = (struct dylib_command *)lc;
		    install_name = (char *)dl + dl->dylib.name.offset;
		    if(names_match(install_name, libraryName) == TRUE)
			return(dl->dylib.current_version);
		}
		lc = (struct load_command *)((char *)lc + lc->cmdsize);
	    }
	}
	return(-1);
}

#if DEPRECATED_APIS_SUPPORTED
/*
 * NSCreateObjectFileImageFromFile() creates an NSObjectFileImage for the
 * specified file name if the file is a correct Mach-O file that can be loaded
 * with NSloadModule().  For return codes of NSObjectFileImageFailure and
 * NSObjectFileImageFormat an error message is printed to stderr.  All
 * other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const char*, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateObjectFileImageFromFile", (void**)&p);
	return p(pathName, objectFileImage);
}


/*
 * NSCreateObjectFileImageFromMemory() creates an NSObjectFileImage for the
 * object file mapped into memory at address of size length if the object file
 * is a correct Mach-O file that can be loaded with NSloadModule().  For return
 * codes of NSObjectFileImageFailure and NSObjectFileImageFormat an error
 * message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromMemory(
const void* address,
size_t size, 
NSObjectFileImage *objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const void*, size_t, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateObjectFileImageFromMemory", (void**)&p);
	return p(address, size, objectFileImage);
}

#if OBSOLETE_DYLD_API
/*
 * NSCreateCoreFileImageFromFile() creates an NSObjectFileImage for the 
 * specified core file name if the file is a correct Mach-O core file.
 * For return codes of NSObjectFileImageFailure and NSObjectFileImageFormat
 * an error message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateCoreFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const char*, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateCoreFileImageFromFile", (void**)&p);
	return p(pathName, objectFileImage);
}
#endif

bool
NSDestroyObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSDestroyObjectFileImage", (void**)&p);
	return p(objectFileImage);
}


NSModule
NSLinkModule(
NSObjectFileImage objectFileImage, 
const char* moduleName,
uint32_t options)
{
	DYLD_LOCK_THIS_BLOCK;
    static NSModule (*p)(NSObjectFileImage, const char*, unsigned long) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLinkModule", (void**)&p);
		
	return p(objectFileImage, moduleName, options);
}




/*
 * NSSymbolDefinitionCountInObjectFileImage() returns the number of symbol
 * definitions in the NSObjectFileImage.
 */
uint32_t
NSSymbolDefinitionCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static unsigned long (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolDefinitionCountInObjectFileImage", (void**)&p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolDefinitionNameInObjectFileImage() returns the name of the i'th
 * symbol definitions in the NSObjectFileImage.  If the ordinal specified is
 * outside the range [0..NSSymbolDefinitionCountInObjectFileImage], NULL will
 * be returned.
 */
const char* 
NSSymbolDefinitionNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal)
{
	DYLD_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSObjectFileImage, uint32_t) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolDefinitionNameInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, ordinal);
}

/*
 * NSSymbolReferenceCountInObjectFileImage() returns the number of references
 * to undefined symbols the NSObjectFileImage.
 */
uint32_t
NSSymbolReferenceCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    static unsigned long (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolReferenceCountInObjectFileImage", (void**)&p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolReferenceNameInObjectFileImage() returns the name of the i'th
 * undefined symbol in the NSObjectFileImage. If the ordinal specified is
 * outside the range [0..NSSymbolReferenceCountInObjectFileImage], NULL will be
 * returned.
 */
const char* 
NSSymbolReferenceNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal,
bool *tentative_definition) /* can be NULL */
{
	DYLD_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSObjectFileImage, uint32_t, bool*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolReferenceNameInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, ordinal, tentative_definition);
}

/*
 * NSIsSymbolDefinedInObjectFileImage() returns TRUE if the specified symbol
 * name has a definition in the NSObjectFileImage and FALSE otherwise.
 */
bool
NSIsSymbolDefinedInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* symbolName)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage, const char*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolDefinedInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, symbolName);
}

/*
 * NSGetSectionDataInObjectFileImage() returns a pointer to the section contents
 * in the NSObjectFileImage for the specified segmentName and sectionName if
 * it exists and it is not a zerofill section.  If not it returns NULL.  If
 * the parameter size is not NULL the size of the section is also returned
 * indirectly through that pointer.
 */
void *
NSGetSectionDataInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* segmentName,
const char* sectionName,
unsigned long *size) /* can be NULL */
{
	DYLD_LOCK_THIS_BLOCK;
    static void* (*p)(NSObjectFileImage, const char*, const char*, unsigned long*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSGetSectionDataInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, segmentName, sectionName, size);
}


void
NSLinkEditError(
NSLinkEditErrors *c,
int *errorNumber, 
const char* *fileName,
const char* *errorString)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(NSLinkEditErrors *c,
		     int *errorNumber, 
		     const char* *fileName,
		     const char* *errorString) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_link_edit_error", (void**)&p);
	if(p != NULL)
	    p(c, errorNumber, fileName, errorString);
}

bool
NSUnLinkModule(
NSModule module, 
uint32_t options)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(NSModule module, uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_unlink_module", (void**)&p);

	return p(module, options);
}

#if OBSOLETE_DYLD_API
NSModule
NSReplaceModule(
NSModule moduleToReplace,
NSObjectFileImage newObjectFileImage, 
uint32_t options)
{
	return(NULL);
}
#endif


#endif // DEPRECATED_APIS_SUPPORTED

/*
 *_NSGetExecutablePath copies the path of the executable into the buffer and
 * returns 0 if the path was successfully copied in the provided buffer. If the
 * buffer is not large enough, -1 is returned and the expected buffer size is
 * copied in *bufsize. Note that _NSGetExecutablePath will return "a path" to
 * the executable not a "real path" to the executable. That is the path may be
 * a symbolic link and not the real file. And with deep directories the total
 * bufsize needed could be more than MAXPATHLEN.
 */
int
_NSGetExecutablePath(
char *buf,
uint32_t *bufsize)
{
	DYLD_LOCK_THIS_BLOCK;
    static int (*p)(char *buf, uint32_t *bufsize) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld__NSGetExecutablePath", (void**)&p);
	return(p(buf, bufsize));
}

#if DEPRECATED_APIS_SUPPORTED
void
_dyld_lookup_and_bind(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(const char*, void** , NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind", (void**)&p);
	p(symbol_name, address, module);
}

void
_dyld_lookup_and_bind_with_hint(
const char* symbol_name,
const char* library_name_hint,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(const char*, const char*, void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_with_hint", (void**)&p);
	p(symbol_name, library_name_hint, address, module);
}

#if OBSOLETE_DYLD_API
void
_dyld_lookup_and_bind_objc(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(const char* , void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_objc", (void**)&p);
	p(symbol_name, address, module);
}
#endif

void
_dyld_lookup_and_bind_fully(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(const char*, void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_fully", (void**)&p);
	p(symbol_name, address, module);
}

bool
_dyld_bind_fully_image_containing_address(
const void* address)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_bind_fully_image_containing_address", (void**)&p);
	return p(address);
}
#endif // DEPRECATED_APIS_SUPPORTED


/*
 * _dyld_register_func_for_add_image registers the specified function to be
 * called when a new image is added (a bundle or a dynamic shared library) to
 * the program.  When this function is first registered it is called for once
 * for each image that is currently part of the program.
 */
void
_dyld_register_func_for_add_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	DYLD_LOCK_THIS_BLOCK;
	typedef void (*callback_t)(const struct mach_header *mh, intptr_t vmaddr_slide);
    static void (*p)(callback_t func) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_add_image", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_remove_image registers the specified function to be
 * called when an image is removed (a bundle or a dynamic shared library) from
 * the program.
 */
void
_dyld_register_func_for_remove_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	DYLD_LOCK_THIS_BLOCK;
	typedef void (*callback_t)(const struct mach_header *mh, intptr_t vmaddr_slide);
    static void (*p)(callback_t func) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_remove_image", (void**)&p);
	p(func);
}

#if OBSOLETE_DYLD_API
/*
 * _dyld_register_func_for_link_module registers the specified function to be
 * called when a module is bound into the program.  When this function is first
 * registered it is called for once for each module that is currently bound into
 * the program.
 */
void
_dyld_register_func_for_link_module(
void (*func)(NSModule module))
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule module)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_link_module", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_unlink_module registers the specified function to be
 * called when a module is unbound from the program.
 */
void
_dyld_register_func_for_unlink_module(
void (*func)(NSModule module))
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule module)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_unlink_module", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_replace_module registers the specified function to be
 * called when a module is to be replace with another module in the program.
 */
void
_dyld_register_func_for_replace_module(
void (*func)(NSModule oldmodule, NSModule newmodule))
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule oldmodule,
				  NSModule newmodule)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_replace_module", (void**)&p);
	p(func);
}


/*
 * _dyld_get_objc_module_sect_for_module is passed a module and sets a 
 * pointer to the (__OBJC,__module) section and its size for the specified
 * module.
 */
void
_dyld_get_objc_module_sect_for_module(
NSModule module,
void **objc_module,
unsigned long *size)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(NSModule module,
		     void **objc_module,
		     unsigned long *size) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_objc_module_sect_for_module", (void**)&p);
	p(module, objc_module, size);
}

/*
 * _dyld_bind_objc_module() is passed a pointer to something in an (__OBJC,
 * __module) section and causes the module that is associated with that address
 * to be bound.
 */
void
_dyld_bind_objc_module(const void* objc_module)
{
	DYLD_LOCK_THIS_BLOCK;
    static void (*p)(const void *objc_module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_bind_objc_module", (void**)&p);
	p(objc_module);
}
#endif

#if DEPRECATED_APIS_SUPPORTED
bool
_dyld_present(void)
{
	// this function exists for compatiblity only
	return true;
}
#endif

uint32_t
_dyld_image_count(void)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static unsigned long (*p)(void) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_image_count", (void**)&p);
	return(p());
}

const struct mach_header *
_dyld_get_image_header(uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static struct mach_header * (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_header", (void**)&p);
	return(p(image_index));
}

intptr_t
_dyld_get_image_vmaddr_slide(uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static unsigned long (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_vmaddr_slide", (void**)&p);
	return(p(image_index));
}

const char* 
_dyld_get_image_name(uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static const char*  (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_name", (void**)&p);
	return(p(image_index));
}

// SPI in Mac OS X 10.6
intptr_t _dyld_get_image_slide(const struct mach_header* mh)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static intptr_t (*p)(const struct mach_header*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_slide", (void**)&p);
	return(p(mh));
}


bool
_dyld_image_containing_address(const void* address)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_image_containing_address", (void**)&p);
	return(p(address));
}

const struct mach_header *
_dyld_get_image_header_containing_address(
const void* address)
{
	DYLD_LOCK_THIS_BLOCK;
    static const struct mach_header * (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_header_containing_address", (void**)&p);
	return p(address);
}

void _dyld_moninit(
void (*monaddition)(char *lowpc, char *highpc))
{
	DYLD_LOCK_THIS_BLOCK;
	typedef void (*monproc)(char *lowpc, char *highpc);
    static void (*p)(monproc monaddition) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_moninit", (void**)&p);
	p(monaddition);
}

#if DEPRECATED_APIS_SUPPORTED
bool _dyld_launched_prebound(void)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(void) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_launched_prebound", (void**)&p);
	return(p());
}

bool _dyld_all_twolevel_modules_prebound(void)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(void) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_all_twolevel_modules_prebound", (void**)&p);
	return(p());
}
#endif // DEPRECATED_APIS_SUPPORTED


#include <dlfcn.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <mach-o/dyld.h>
#include <servers/bootstrap.h>
#include "dyldLibSystemInterface.h"


// pthread key used to access per-thread dlerror message
static pthread_key_t dlerrorPerThreadKey;
static bool dlerrorPerThreadKeyInitialized = false;

// data kept per-thread
struct dlerrorPerThreadData
{
	uint32_t	sizeAllocated;
	char		message[1];
};

// function called by dyld to get buffer to store dlerror message
static char* getPerThreadBufferFor_dlerror(uint32_t sizeRequired)
{
	// ok to create key lazily because this function is called within dyld lock, so there is no race condition
	if (!dlerrorPerThreadKeyInitialized ) {
		// create key and tell pthread package to call free() on any data associated with key if thread dies
		pthread_key_create(&dlerrorPerThreadKey, &free);
		dlerrorPerThreadKeyInitialized = true;
	}

	const int size = (sizeRequired < 256) ? 256 : sizeRequired;
	dlerrorPerThreadData* data = (dlerrorPerThreadData*)pthread_getspecific(dlerrorPerThreadKey);
	if ( data == NULL ) {
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const int mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	else if ( data->sizeAllocated < sizeRequired ) {
		free(data);
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const int mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	return data->message;
}


#if DYLD_SHARED_CACHE_SUPPORT
static void shared_cache_missing()
{
	// leave until dyld's that might call this are rare
}

static void shared_cache_out_of_date()
{
	// leave until dyld's that might call this are rare
}
#endif // DYLD_SHARED_CACHE_SUPPORT


// the table passed to dyld containing thread helpers
static dyld::LibSystemHelpers sHelpers = { 8, &dyldGlobalLockAcquire, &dyldGlobalLockRelease,  
									&getPerThreadBufferFor_dlerror, &malloc, &free, &__cxa_atexit,
						#if DYLD_SHARED_CACHE_SUPPORT
									&shared_cache_missing, &shared_cache_out_of_date,
						#else
									NULL, NULL,
						#endif
									NULL, NULL,
									&pthread_key_create, &pthread_setspecific,
									&malloc_size,
									&pthread_getspecific,
									&__cxa_finalize};


//
// during initialization of libSystem this routine will run
// and call dyld, registering the helper functions.
//
extern "C" void tlv_initializer();
extern "C" void _dyld_initializer();
void _dyld_initializer()
{
	DYLD_LOCK_INITIALIZER;
	
   void (*p)(dyld::LibSystemHelpers*);

	_dyld_func_lookup("__dyld_register_thread_helpers", (void**)&p);
	if(p != NULL)
		p(&sHelpers);
		
	tlv_initializer();
}


char* dlerror()
{
	DYLD_LOCK_THIS_BLOCK;
    static char* (*p)() = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlerror", (void**)&p);
	return(p());
}

int dladdr(const void* addr, Dl_info* info)
{
	DYLD_LOCK_THIS_BLOCK;
    static int (*p)(const void* , Dl_info*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dladdr", (void**)&p);
	return(p(addr, info));
}

int dlclose(void* handle)
{
	DYLD_LOCK_THIS_BLOCK;
    static int (*p)(void* handle) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlclose", (void**)&p);
	return(p(handle));
}

void* dlopen(const char* path, int mode)
{	
	// dlopen is special. locking is done inside dyld to allow initializer to run without lock
	DYLD_NO_LOCK_THIS_BLOCK;
	
    static void* (*p)(const char* path, int) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlopen", (void**)&p);
	void* result = p(path, mode);
	// use asm block to prevent tail call optimization
	// this is needed because dlopen uses __builtin_return_address() and depends on this glue being in the frame chain
	// <rdar://problem/5313172 dlopen() looks too far up stack, can cause crash>
	__asm__ volatile(""); 
	
	return result;
}

bool dlopen_preflight(const char* path)
{
	DYLD_LOCK_THIS_BLOCK;
    static bool (*p)(const char* path) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlopen_preflight", (void**)&p);
	return(p(path));
}

void* dlsym(void* handle, const char* symbol)
{
	DYLD_LOCK_THIS_BLOCK;
    static void* (*p)(void* handle, const char* symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlsym", (void**)&p);
	return(p(handle, symbol));
}

void dyld_register_image_state_change_handler(dyld_image_states state, 
											bool batch, dyld_image_state_change_handler handler)
{
	DYLD_LOCK_THIS_BLOCK;
    static void* (*p)(dyld_image_states, bool, dyld_image_state_change_handler) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dyld_register_image_state_change_handler", (void**)&p);
	p(state, batch, handler);
}


const struct dyld_all_image_infos* _dyld_get_all_image_infos()
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static struct dyld_all_image_infos* (*p)() = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_all_image_infos", (void**)&p);
	return p();
}

#if !__arm__
bool _dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static void* (*p)(void*, dyld_unwind_sections*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_find_unwind_sections", (void**)&p);
	return p(addr, info);
}
#endif


#if __i386__ || __x86_64__ || __arm__
__attribute__((visibility("hidden"))) 
void* _dyld_fast_stub_entry(void* loadercache, long lazyinfo)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static void* (*p)(void*, long) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_fast_stub_entry", (void**)&p);
	return p(loadercache, lazyinfo);
}
#endif


const char* dyld_image_path_containing_address(const void* addr)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static const char* (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_image_path_containing_address", (void**)&p);
	return p(addr);
}

#if __IPHONE_OS_VERSION_MIN_REQUIRED	
bool dyld_shared_cache_some_image_overridden()
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static bool (*p)() = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_shared_cache_some_image_overridden", (void**)&p);
	return p();
}
#endif


// SPI called __fork
void _dyld_fork_child()
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static void (*p)() = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_fork_child", (void**)&p);
	return p();
}




