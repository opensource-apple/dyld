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

//
// This file implements that API's in <mach-o/dyld.h>
//
//


#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <sys/time.h>
#include <sys/sysctl.h>

extern "C" mach_port_name_t	task_self_trap(void);  // can't include <System/mach/mach_traps.h> because it is missing extern C

#include "mach-o/dyld_gdb.h"
#include "mach-o/dyld.h"
#include "mach-o/dyld_priv.h"
#include "dlfcn.h"

#include "ImageLoader.h"
#include "dyld.h"
#include "dyldLibSystemThreadHelpers.h"

static char sLastErrorFilePath[1024];
static NSLinkEditErrors sLastErrorFileCode;
static int sLastErrorNo;


// In 10.3.x and earlier all the NSObjectFileImage API's were implemeneted in libSystem.dylib
// Beginning in 10.4 the NSObjectFileImage API's are implemented in dyld and libSystem just forwards
// This conditional keeps support for old libSystem's which needed some help implementing the API's
#define OLD_LIBSYSTEM_SUPPORT 1


// The following functions have no prototype in any header.  They are special cases
// where _dyld_func_lookup() is used directly.
static void _dyld_install_handlers(void* undefined, void* multiple, void* linkEdit);
static NSModule _dyld_link_module(NSObjectFileImage object_addr, size_t object_size, const char* moduleName, uint32_t options);
static void _dyld_register_binding_handler(void * (*)(const char *, const char *, void *), ImageLoader::BindingOptions);
static void _dyld_fork_prepare();
static void _dyld_fork_parent();
static void _dyld_fork_child();
static void _dyld_fork_child_final();
static void _dyld_make_delayed_module_initializer_calls();
static void _dyld_mod_term_funcs();
static bool NSMakePrivateModulePublic(NSModule module);
static void _dyld_call_module_initializers_for_dylib(const struct mach_header* mh_dylib_header);
static void registerThreadHelpers(const dyld::ThreadingHelpers*);
static void _dyld_update_prebinding(int pathCount, const char* paths[], uint32_t flags);
static const struct dyld_all_image_infos* _dyld_get_all_image_infos();

// The following functions are dyld API's, but since dyld links with a static copy of libc.a
// the public name cannot be used.
static void		client_dyld_lookup_and_bind(const char* symbolName, void** address, NSModule* module);
static bool		client_NSIsSymbolNameDefined(const char* symbolName);


static void unimplemented()
{
	dyld::halt("unimplemented dyld function\n");
}

struct dyld_func {
    const char* name;
    void*		implementation;
};

static struct dyld_func dyld_funcs[] = {
    {"__dyld_image_count",							(void*)_dyld_image_count },
    {"__dyld_get_image_header",						(void*)_dyld_get_image_header },
    {"__dyld_get_image_vmaddr_slide",				(void*)_dyld_get_image_vmaddr_slide },
    {"__dyld_get_image_name",						(void*)_dyld_get_image_name },
    {"__dyld_lookup_and_bind",						(void*)client_dyld_lookup_and_bind },
    {"__dyld_lookup_and_bind_with_hint",			(void*)_dyld_lookup_and_bind_with_hint },
    {"__dyld_lookup_and_bind_objc",					(void*)unimplemented },
    {"__dyld_lookup_and_bind_fully",				(void*)_dyld_lookup_and_bind_fully },
    {"__dyld_install_handlers",						(void*)_dyld_install_handlers },
    {"__dyld_link_edit_error",						(void*)NSLinkEditError },
#if OLD_LIBSYSTEM_SUPPORT
    {"__dyld_link_module",							(void*)_dyld_link_module },
#endif
    {"__dyld_unlink_module",						(void*)NSUnLinkModule },
    {"__dyld_register_func_for_add_image",			(void*)_dyld_register_func_for_add_image },
    {"__dyld_register_func_for_remove_image",		(void*)_dyld_register_func_for_remove_image },
    {"__dyld_register_func_for_link_module",		(void*)unimplemented },
    {"__dyld_register_func_for_unlink_module",		(void*)unimplemented },
    {"__dyld_register_func_for_replace_module",		(void*)unimplemented },
    {"__dyld_get_objc_module_sect_for_module",		(void*)unimplemented },
    {"__dyld_bind_objc_module",						(void*)_dyld_bind_objc_module },
    {"__dyld_bind_fully_image_containing_address",  (void*)_dyld_bind_fully_image_containing_address },
    {"__dyld_image_containing_address",				(void*)_dyld_image_containing_address },
    {"__dyld_get_image_header_containing_address",  (void*)_dyld_get_image_header_containing_address },
    {"__dyld_moninit",								(void*)_dyld_moninit },
    {"__dyld_register_binding_handler",						(void*)_dyld_register_binding_handler },
    {"__dyld_fork_prepare",							(void*)_dyld_fork_prepare },
    {"__dyld_fork_parent",							(void*)_dyld_fork_parent },
    {"__dyld_fork_child",							(void*)_dyld_fork_child },
    {"__dyld_fork_child_final",						(void*)_dyld_fork_child_final },
    {"__dyld_fork_mach_init",						(void*)unimplemented },
    {"__dyld_make_delayed_module_initializer_calls",(void*)_dyld_make_delayed_module_initializer_calls },
    {"__dyld_NSNameOfSymbol",						(void*)NSNameOfSymbol },
    {"__dyld_NSAddressOfSymbol",					(void*)NSAddressOfSymbol },
    {"__dyld_NSModuleForSymbol",					(void*)NSModuleForSymbol },
    {"__dyld_NSLookupAndBindSymbol",				(void*)NSLookupAndBindSymbol },
    {"__dyld_NSLookupAndBindSymbolWithHint",		(void*)NSLookupAndBindSymbolWithHint },
    {"__dyld_NSLookupSymbolInModule",				(void*)NSLookupSymbolInModule},
    {"__dyld_NSLookupSymbolInImage",				(void*)NSLookupSymbolInImage},
    {"__dyld_NSMakePrivateModulePublic",			(void*)NSMakePrivateModulePublic},
    {"__dyld_NSIsSymbolNameDefined",				(void*)client_NSIsSymbolNameDefined},
    {"__dyld_NSIsSymbolNameDefinedWithHint",		(void*)NSIsSymbolNameDefinedWithHint },
    {"__dyld_NSIsSymbolNameDefinedInImage",			(void*)NSIsSymbolNameDefinedInImage},
    {"__dyld_NSNameOfModule",						(void*)NSNameOfModule },
    {"__dyld_NSLibraryNameForModule",				(void*)NSLibraryNameForModule },
    {"__dyld_NSAddLibrary",							(void*)NSAddLibrary },
    {"__dyld_NSAddLibraryWithSearching",			(void*)NSAddLibraryWithSearching },
    {"__dyld_NSAddImage",							(void*)NSAddImage },
    {"__dyld__NSGetExecutablePath",					(void*)_NSGetExecutablePath },
    {"__dyld_launched_prebound",					(void*)_dyld_launched_prebound },
    {"__dyld_all_twolevel_modules_prebound",		(void*)_dyld_all_twolevel_modules_prebound },
    {"__dyld_call_module_initializers_for_dylib",   (void*)_dyld_call_module_initializers_for_dylib },
    {"__dyld_mod_term_funcs",						(void*)_dyld_mod_term_funcs },
    {"__dyld_install_link_edit_symbol_handlers",	(void*)dyld::registerZeroLinkHandlers },
    {"__dyld_NSCreateObjectFileImageFromFile",			(void*)NSCreateObjectFileImageFromFile },
    {"__dyld_NSCreateObjectFileImageFromMemory",		(void*)NSCreateObjectFileImageFromMemory },
    {"__dyld_NSCreateCoreFileImageFromFile",			(void*)unimplemented },
    {"__dyld_NSDestroyObjectFileImage",					(void*)NSDestroyObjectFileImage },
    {"__dyld_NSLinkModule",								(void*)NSLinkModule },
    {"__dyld_NSHasModInitObjectFileImage",				(void*)NSHasModInitObjectFileImage },
    {"__dyld_NSSymbolDefinitionCountInObjectFileImage",	(void*)NSSymbolDefinitionCountInObjectFileImage },
    {"__dyld_NSSymbolDefinitionNameInObjectFileImage",	(void*)NSSymbolDefinitionNameInObjectFileImage },
    {"__dyld_NSIsSymbolDefinedInObjectFileImage",		(void*)NSIsSymbolDefinedInObjectFileImage },
    {"__dyld_NSSymbolReferenceNameInObjectFileImage",	(void*)NSSymbolReferenceNameInObjectFileImage },
    {"__dyld_NSSymbolReferenceCountInObjectFileImage",	(void*)NSSymbolReferenceCountInObjectFileImage },
    {"__dyld_NSGetSectionDataInObjectFileImage",		(void*)NSGetSectionDataInObjectFileImage },
    {"__dyld_NSFindSectionAndOffsetInObjectFileImage",  (void*)NSFindSectionAndOffsetInObjectFileImage },
	{"__dyld_register_thread_helpers",					(void*)registerThreadHelpers },
    {"__dyld_dladdr",									(void*)dladdr },
    {"__dyld_dlclose",									(void*)dlclose },
    {"__dyld_dlerror",									(void*)dlerror },
    {"__dyld_dlopen",									(void*)dlopen },
    {"__dyld_dlsym",									(void*)dlsym },
	{"__dyld_update_prebinding",						(void*)_dyld_update_prebinding },
	{"__dyld_get_all_image_infos",						(void*)_dyld_get_all_image_infos },
    {NULL, 0}
};



// dyld's abstract type NSSymbol is implemented as const ImageLoader::Symbol*
inline NSSymbol SymbolToNSSymbol(const ImageLoader::Symbol* sym)
{
	return (NSSymbol)sym;
}	
inline const ImageLoader::Symbol* NSSymbolToSymbol(NSSymbol sym)
{
	return (const ImageLoader::Symbol*)sym;
}	

// dyld's abstract type NSModule is implemented as ImageLoader*
inline NSModule ImageLoaderToNSModule(ImageLoader* image)
{
	return (NSModule)image;
}	
inline ImageLoader* NSModuleToImageLoader(NSModule module)
{
	ImageLoader* image = (ImageLoader*)module;
	if ( dyld::validImage(image) )
		return image;
	return NULL;
}

// actual definition for opaque type
struct __NSObjectFileImage
{
	ImageLoader*	image;	
	const void*		imageBaseAddress;	// not used with OFI created from files
	size_t			imageLength;		// not used with OFI created from files
};
static std::vector<NSObjectFileImage> sObjectFileImages;



//
// __NSObjectFileImage are deleted in NSDestroyObjectFileImage()
// The contained image is delete in one of two places:
//	NSUnLinkModule deletes the image if there is no __NSObjectFileImage with a reference to it
//	NSDestroyObjectFileImage deletes the image if image is not in list of valid images
//


static void dyldAPIhalt(const char* apiName, const char* errorMsg)
{
	fprintf(stderr, "dyld: %s() error\n", apiName);
	dyld::halt(errorMsg);
}



static void setLastError(NSLinkEditErrors code, int errnum, const char* file, const char* message)
{
	dyld::setErrorMessage(message);
	strncpy(sLastErrorFilePath, file, 1024);
	sLastErrorFilePath[1023] = '\0';
	sLastErrorFileCode = code;
	sLastErrorNo = errnum;
}


/*
 *_dyld_NSGetExecutablePath is the dyld side of _NSGetExecutablePath which
 * copies the path of the executable into the buffer and returns 0 if the path
 * was successfully copied in the provided buffer. If the buffer is not large
 * enough, -1 is returned and the expected buffer size is copied in *bufsize.
 * Note that _NSGetExecutablePath will return "a path" to the executable not a
 * "real path" to the executable. That is the path may be a symbolic link and
 * not the real file. And with deep directories the total bufsize needed could
 * be more than MAXPATHLEN.
 */
int _NSGetExecutablePath(char* buf, uint32_t *bufsize)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(...)\n", __func__);
	const char* exePath = dyld::getExecutablePath();
	if(*bufsize < strlen(exePath) + 1){
	    *bufsize = strlen(exePath) + 1;
	    return -1;
	}
	strcpy(buf, exePath);
	return 0;
}


//
// _dyld_call_module_initializers_for_dylib() is the dyld side of
// __initialize_Cplusplus() which is in dylib1.o.
// It is intended to only be called inside -init rouintes.
// -init routines are called before module initializers (what C++
// initializers use).  Calling __initialize_Cplusplus() in a -init
// routine causes the module initializers for an image to be called
// which then allows C++ to be used inside a -init routine
//
static void _dyld_call_module_initializers_for_dylib(const struct mach_header* mh_dylib_header)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "__initialize_Cplusplus()\n");
	
	// for now, do nothing...
}


void _dyld_lookup_and_bind_fully(const char* symbolName, void** address, NSModule* module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", %p, %p)\n", __func__, symbolName, address, module);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	dyld::clearErrorMessage();
	if ( dyld::flatFindExportedSymbol(symbolName, &sym, &image) ) {	
		try {
			dyld::link(image, ImageLoader::kLazyOnly, ImageLoader::kDontRunInitializers);
			if ( address != NULL)
				*address = (void*)image->getExportedSymbolAddress(sym);
			if ( module != NULL)
				*module = ImageLoaderToNSModule(image);
		}
		catch (const char* msg) {
			dyldAPIhalt(__func__, msg);
		}
	}
	else {
		// on failure to find symbol return NULLs
		if ( address != NULL)
			*address = NULL;
		if ( module != NULL)
			*module = NULL;
	}
}

// Note: This cannot have public name because dyld is built with a static copy of libc.a
// which calls dyld_lookup_and_bind() and expects to find dyld's symbols not host process
static void client_dyld_lookup_and_bind(const char* symbolName, void** address, NSModule* module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "_dyld_lookup_and_bind(\"%s\", %p, %p)\n", symbolName, address, module);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	if ( dyld::flatFindExportedSymbol(symbolName, &sym, &image) ) {
		if ( address != NULL)
			*address = (void*)image->getExportedSymbolAddress(sym);
		if ( module != NULL)
			*module = ImageLoaderToNSModule(image);
	}
	else {
		// on failure to find symbol return NULLs
		if ( address != NULL)
			*address = NULL;
		if ( module != NULL)
			*module = NULL;
	}
}

void _dyld_lookup_and_bind_with_hint(const char* symbolName, const char* library_name_hint, void** address, NSModule* module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", \"%s\", %p, %p)\n", __func__, symbolName, library_name_hint, address, module);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	// Look for library whose path contains the hint.  If that fails search everywhere
	if (  dyld::flatFindExportedSymbolWithHint(symbolName, library_name_hint, &sym, &image) 
	  ||  dyld::flatFindExportedSymbol(symbolName, &sym, &image) ) {
		if ( address != NULL)
			*address = (void*)image->getExportedSymbolAddress(sym);
		if ( module != NULL)
			*module = ImageLoaderToNSModule(image);
	}
	else {
		// on failure to find symbol return NULLs
		if ( address != NULL)
			*address = NULL;
		if ( module != NULL)
			*module = NULL;
	}
}


NSSymbol NSLookupAndBindSymbol(const char *symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\")\n", __func__, symbolName);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	if ( dyld::flatFindExportedSymbol(symbolName, &sym, &image) ) {
		return SymbolToNSSymbol(sym);
	}
	// return NULL on failure
	return NULL;
}

NSSymbol NSLookupAndBindSymbolWithHint(const char* symbolName, const char* libraryNameHint)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", \"%s\")\n", __func__, symbolName, libraryNameHint);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	bool found = dyld::flatFindExportedSymbolWithHint(symbolName, libraryNameHint, &sym, &image);
	if ( ! found ) {
		// hint failed, do slow search of all images
		 found = dyld::flatFindExportedSymbol(symbolName, &sym, &image);
	}
	if ( found )
		return SymbolToNSSymbol(sym);
		
	// return NULL on failure and log
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", \"%s\") => NULL \n", __func__, symbolName, libraryNameHint);
	return NULL;
}

uint32_t _dyld_image_count(void)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	return dyld::getImageCount();
}

const struct mach_header* _dyld_get_image_header(uint32_t image_index)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%u)\n", __func__, image_index);
	ImageLoader* image = dyld::getIndexedImage(image_index);
	if ( image != NULL )
		return (struct mach_header*)image->machHeader();
	else
		return NULL;
}


static __attribute__((noinline)) 
const struct mach_header* addImage(void* callerAddress, const char* path, bool search, bool dontLoad, bool matchInstallName, bool abortOnError)
{
	ImageLoader*	image = NULL;
	try {
		dyld::clearErrorMessage();
		ImageLoader* callerImage = dyld::findImageContainingAddress(callerAddress);
		dyld::LoadContext context;
		context.useSearchPaths		= search;
		context.useLdLibraryPath	= false;
		context.matchByInstallName	= matchInstallName;
		context.dontLoad			= dontLoad;
		context.mustBeBundle		= false;
		context.mustBeDylib			= true;
		context.origin				= callerImage != NULL ? callerImage->getPath() : NULL; // caller's image's path
		context.rpath				= NULL; // support not yet implemented
				
		image = load(path, context);
		if ( image != NULL ) {
			if ( context.matchByInstallName )
				image->setMatchInstallPath(true);
			dyld::link(image, ImageLoader::kNonLazyOnly, ImageLoader::kRunInitializers);
			return image->machHeader();
		}
	}
	catch (const char* msg) {
		if ( abortOnError) {
			char pathMsg[strlen(msg)+strlen(path)+4];
			strcpy(pathMsg, msg);
			strcat(pathMsg, " ");
			strcat(pathMsg, path);
			dyldAPIhalt("NSAddImage", pathMsg);
		}
		// not halting, so set error state for NSLinkEditError to find
		setLastError(NSLinkEditOtherError, 0, path, msg);
	}
	return NULL;
}

const struct mach_header* NSAddImage(const char* path, uint32_t options)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", 0x%08X)\n", __func__, path, options);
	const bool dontLoad = ( (options & NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED) != 0 );
	const bool search = ( (options & NSADDIMAGE_OPTION_WITH_SEARCHING) != 0 );
	const bool matchInstallName = ( (options & NSADDIMAGE_OPTION_MATCH_FILENAME_BY_INSTALLNAME) != 0 );
	const bool abortOnError = ( (options & NSADDIMAGE_OPTION_RETURN_ON_ERROR) == 0 );
	void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
	return addImage(callerAddress, path, search, dontLoad, matchInstallName, abortOnError);
}

bool NSAddLibrary(const char* path)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\")\n", __func__, path);
	void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
	return (addImage(callerAddress, path, false, false, false, false) != NULL);
}

bool NSAddLibraryWithSearching(const char* path)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\")\n", __func__, path);
	void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
	return (addImage(callerAddress, path, true, false, false, false) != NULL);
}



//#define NSADDIMAGE_OPTION_NONE                  	0x0
//#define NSADDIMAGE_OPTION_RETURN_ON_ERROR       	0x1
//#define NSADDIMAGE_OPTION_MATCH_FILENAME_BY_INSTALLNAME	0x8

bool NSIsSymbolNameDefinedInImage(const struct mach_header* mh, const char* symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, \"%s\")\n", __func__, (void *)mh, symbolName);
	ImageLoader* image = dyld::findImageByMachHeader(mh);
	if ( image != NULL ) {
		if ( image->findExportedSymbol(symbolName, NULL, true, NULL) != NULL)
			return true;
	}
	return false;
}

NSSymbol NSLookupSymbolInImage(const struct mach_header* mh, const char* symbolName, uint32_t options)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, \"%s\", 0x%08X)\n", __func__, mh, symbolName, options);
	const ImageLoader::Symbol* symbol = NULL;
	dyld::clearErrorMessage();
	ImageLoader* image = dyld::findImageByMachHeader(mh);
	if ( image != NULL ) {
		try {
			if ( options & NSLOOKUPSYMBOLINIMAGE_OPTION_BIND_FULLY ) {
				dyld::link(image, ImageLoader::kLazyOnly, ImageLoader::kDontRunInitializers);
			}
			else if ( options & NSLOOKUPSYMBOLINIMAGE_OPTION_BIND_NOW ) {
				dyld::link(image, ImageLoader::kLazyOnlyNoDependents, ImageLoader::kDontRunInitializers);
			}
		}
		catch (const char* msg) {
			if ( (options & NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR) == 0 ) {
				dyldAPIhalt(__func__, msg);
			}
		}
		symbol = image->findExportedSymbol(symbolName, NULL, true, NULL);
	}
	if ( dyld::gLogAPIs && (symbol == NULL) )
		fprintf(stderr, "%s(%p, \"%s\", 0x%08X) ==> NULL\n", __func__, mh, symbolName, options);
	return SymbolToNSSymbol(symbol);
}


// Note: This cannot have public name because dyld is built with a static copy of libc.a
// which calls NSIsSymbolNameDefined() and expects to find dyld's symbols not host process
static bool client_NSIsSymbolNameDefined(const char* symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "NSIsSymbolNameDefined(\"%s\")\n", symbolName);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	return dyld::flatFindExportedSymbol(symbolName, &sym, &image);
}

bool NSIsSymbolNameDefinedWithHint(const char* symbolName, const char* libraryNameHint)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", \"%s\")\n", __func__, symbolName, libraryNameHint);
	ImageLoader* image;
	const ImageLoader::Symbol* sym;
	bool found = dyld::flatFindExportedSymbolWithHint(symbolName, libraryNameHint, &sym, &image);
	if ( ! found ) {
		// hint failed, do slow search of all images
		 found = dyld::flatFindExportedSymbol(symbolName, &sym, &image);
	}
	if ( !found && dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", \"%s\") => false \n", __func__, symbolName, libraryNameHint);
	return found;
}

const char* NSNameOfSymbol(NSSymbol symbol)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, (void *)symbol);
	const char* result = NULL;
	ImageLoader* image = dyld::findImageContainingAddress(symbol);
	if ( image != NULL ) 
		result = image->getExportedSymbolName(NSSymbolToSymbol(symbol));
	return result;
}

void* NSAddressOfSymbol(NSSymbol symbol)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, (void *)symbol);
	void* result = NULL;
	ImageLoader* image = dyld::findImageContainingAddress(symbol);
	if ( image != NULL ) 
		result = (void*)image->getExportedSymbolAddress(NSSymbolToSymbol(symbol));
	return result;
}

NSModule NSModuleForSymbol(NSSymbol symbol)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, (void *)symbol);
	NSModule result = NULL;
	ImageLoader* image = dyld::findImageContainingAddress(symbol);
	if ( image != NULL ) 
		result = ImageLoaderToNSModule(image);
	return result;
}


intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%u)\n", __func__, image_index);
	ImageLoader* image = dyld::getIndexedImage(image_index);
	if ( image != NULL )
		return image->getSlide();
	else
		return 0;
}

const char* _dyld_get_image_name(uint32_t image_index)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%u)\n", __func__, image_index);
	ImageLoader* image = dyld::getIndexedImage(image_index);
	if ( image != NULL )
		return image->getLogicalPath();
	else
		return NULL;
}



bool _dyld_all_twolevel_modules_prebound(void)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	return FALSE; // fixme
}

void _dyld_bind_objc_module(const void *objc_module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objc_module);
	// do nothing, with new dyld everything already bound
}


bool _dyld_bind_fully_image_containing_address(const void* address)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, address);
	dyld::clearErrorMessage();
	ImageLoader* image = dyld::findImageContainingAddress(address);
	if ( image != NULL ) {
		try {
			dyld::link(image, ImageLoader::kLazyAndNonLazy, ImageLoader::kDontRunInitializers);
			return true;
		}
		catch (const char* msg) {
			dyldAPIhalt(__func__, msg);
		}
	}
	return false;
}

bool _dyld_image_containing_address(const void* address)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, address);
	ImageLoader *imageLoader = dyld::findImageContainingAddress(address);
	return (NULL != imageLoader);
}

static NSObjectFileImage createObjectImageFile(ImageLoader* image, const void* address = NULL, size_t len=0)
{
	NSObjectFileImage result = new __NSObjectFileImage();
	result->image = image;
	result->imageBaseAddress = address;
	result->imageLength = len;
	sObjectFileImages.push_back(result);
	return result;
}

NSObjectFileImageReturnCode NSCreateObjectFileImageFromFile(const char* pathName, NSObjectFileImage *objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(\"%s\", ...)\n", __func__, pathName);
	try {
		void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
		ImageLoader* callerImage = dyld::findImageContainingAddress(callerAddress);

		dyld::LoadContext context;
		context.useSearchPaths		= false;
		context.useLdLibraryPath	= false;
		context.matchByInstallName	= false;
		context.dontLoad			= false;
		context.mustBeBundle		= true;
		context.mustBeDylib			= false;
		context.origin				= callerImage != NULL ? callerImage->getPath() : NULL; // caller's image's path
		context.rpath				= NULL; // support not yet implemented

		ImageLoader* image = dyld::load(pathName, context);
		// Note:  We DO NOT link the image!  NSLinkModule will do that
		if ( image != NULL ) {
			if ( !image->isBundle() ) {
				// the image must have been already loaded (since context.mustBeBundle will prevent it from being loaded)
				return NSObjectFileImageInappropriateFile;
			}
			*objectFileImage = createObjectImageFile(image);
			return NSObjectFileImageSuccess;
		}
	}
	catch (const char* msg) {
		//fprintf(stderr, "dyld: NSCreateObjectFileImageFromFile() error: %s\n", msg);
		return NSObjectFileImageInappropriateFile;
	}
	return NSObjectFileImageFailure;
}


NSObjectFileImageReturnCode NSCreateObjectFileImageFromMemory(const void* address, size_t size, NSObjectFileImage *objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, %lu, %p)\n", __func__, address, size, objectFileImage);
	
	try {
		ImageLoader* image = dyld::loadFromMemory((const uint8_t*)address, size, NULL); 
		if ( ! image->isBundle() ) {
			// this API can only be used with bundles...
			delete image; 
			return NSObjectFileImageInappropriateFile;
		}
		// Note:  We DO NOT link the image!  NSLinkModule will do that
		if ( image != NULL ) {
			*objectFileImage = createObjectImageFile(image, address, size);
			return NSObjectFileImageSuccess;
		}
	}
	catch (const char* msg) {
		fprintf(stderr, "dyld: NSCreateObjectFileImageFromMemory() error: %s\n", msg);
	}
	return NSObjectFileImageFailure;
}

static bool validOFI(NSObjectFileImage objectFileImage)
{
	const int ofiCount = sObjectFileImages.size();
	for (int i=0; i < ofiCount; ++i) {
		if ( sObjectFileImages[i] == objectFileImage )
			return true;
	}
	return false;
}

bool NSDestroyObjectFileImage(NSObjectFileImage objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objectFileImage);
	
	if ( validOFI(objectFileImage) ) {
		// if the image has never been linked or has been unlinked, the image is not in the list of valid images
		// and we should delete it
		bool linkedImage = dyld::validImage(objectFileImage->image);
		if ( ! linkedImage ) 
			delete objectFileImage->image;
		
		// remove from list of ofi's
		for (std::vector<NSObjectFileImage>::iterator it=sObjectFileImages.begin(); it != sObjectFileImages.end(); it++) {
			if ( *it == objectFileImage ) {
				sObjectFileImages.erase(it);
				break;
			}
		}
		
		// if object was created from a memory, release that memory
		// NOTE: this is the way dyld has always done this. NSCreateObjectFileImageFromMemory() hands over ownership of the memory to dyld
		if ( objectFileImage->imageBaseAddress != NULL ) {
			vm_deallocate(mach_task_self(), (vm_address_t)objectFileImage->imageBaseAddress, objectFileImage->imageLength);
		}
		
		// free ofi object
		delete objectFileImage;
		
		return true;
	}
	return false;
}

bool NSHasModInitObjectFileImage(NSObjectFileImage objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objectFileImage);
	return objectFileImage->image->needsInitialization();
}

uint32_t NSSymbolDefinitionCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objectFileImage);
	return objectFileImage->image->getExportedSymbolCount();
}

const char* NSSymbolDefinitionNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p,%d)\n", __func__, objectFileImage, ordinal);
	const ImageLoader::Symbol* sym = objectFileImage->image->getIndexedExportedSymbol(ordinal);
	return objectFileImage->image->getExportedSymbolName(sym);	
}

uint32_t NSSymbolReferenceCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objectFileImage);
	return objectFileImage->image->getImportedSymbolCount();
}

const char * NSSymbolReferenceNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal, 
													bool* tentative_definition)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p,%d)\n", __func__, objectFileImage, ordinal);
	const ImageLoader::Symbol* sym = objectFileImage->image->getIndexedImportedSymbol(ordinal);
	if ( tentative_definition != NULL ) {
		ImageLoader::ReferenceFlags flags = objectFileImage->image->geImportedSymbolInfo(sym);
		if ( (flags & ImageLoader::kTentativeDefinition) != 0 )
			*tentative_definition = true;
		else
			*tentative_definition = false;
	}
	return objectFileImage->image->getImportedSymbolName(sym);	
}

void* NSGetSectionDataInObjectFileImage(NSObjectFileImage objectFileImage,
										const char* segmentName, const char* sectionName, unsigned long* size)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p,%s, %s)\n", __func__, objectFileImage, segmentName, sectionName);

	void* start;
	size_t length;
	if ( objectFileImage->image->getSectionContent(segmentName, sectionName, &start, &length) ) {
		if ( size != NULL )
			*size = length;
		return start;
	}
	return NULL;
}



bool NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage objectFileImage, const char* symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p,%s)\n", __func__, objectFileImage, symbolName);
	const ImageLoader::Symbol* sym = objectFileImage->image->findExportedSymbol(symbolName, NULL, true, NULL);
	return ( sym != NULL );
}

/*
 * Given an imageOffset into an ObjectFileImage, returns 
 * the segment/section name and offset into that section of
 * that imageOffset.  Returns FALSE if the imageOffset is not 
 * in any section.  You can used the resulting sectionOffset to
 * index into the data returned by NSGetSectionDataInObjectFileImage.
 * 
 * First appeared in Mac OS X 10.3 
 *
 * SPI: currently only used by ZeroLink to detect +load methods
 */
bool 
NSFindSectionAndOffsetInObjectFileImage(NSObjectFileImage objectFileImage, 
										unsigned long imageOffset,
										const char** segmentName, 	/* can be NULL */
										const char** sectionName, 	/* can be NULL */
										unsigned long* sectionOffset)	/* can be NULL */
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, objectFileImage);
	
	return objectFileImage->image->findSection((char*)(objectFileImage->image->getBaseAddress())+imageOffset, segmentName, sectionName, sectionOffset);
}



NSModule NSLinkModule(NSObjectFileImage objectFileImage, const char* moduleName, uint32_t options)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, \"%s\", 0x%08X)\n", __func__, objectFileImage, moduleName, options); 
	
	dyld::clearErrorMessage();
	try {
		// NSLinkModule allows a bundle to be link multpile times
		// each link causes the bundle to be copied to a new address
		if ( objectFileImage->image->isLinked() ) {
			// already linked, so clone a new one and link it
#if 0
			fprintf(stderr, "dyld: warning: %s(0x%08X, \"%s\", 0x%08X) called more than once for 0x%08X\n",
					__func__, objectFileImage, moduleName, options, objectFileImage); 
#endif
			objectFileImage->image = dyld::cloneImage(objectFileImage->image);
		}
		
		// if this ofi was made with NSCreateObjectFileImageFromFile() then physical path is already set
		// if this ofi was create with NSCreateObjectFileImageFromMemory() then the phyiscal path should be set if supplied 
		if ( (options & NSLINKMODULE_OPTION_TRAILING_PHYS_NAME) != 0 ) {
			if ( objectFileImage->imageBaseAddress != NULL ) {
				const char* physEnd = &moduleName[strlen(moduleName)+1];
				objectFileImage->image->setPath(physEnd);
			}
		}
	
		// set moduleName as the name anyone calling _dyld_get_image_name() will see
		objectFileImage->image->setLogicalPath(moduleName);

		// support private bundles
		if ( (options & NSLINKMODULE_OPTION_PRIVATE) != 0 )
			objectFileImage->image->setHideExports();
	
		// set up linking options
		ImageLoader::BindingLaziness bindness = ImageLoader::kNonLazyOnly;
		if ( (options & NSLINKMODULE_OPTION_BINDNOW) != 0 )
			bindness = ImageLoader::kLazyAndNonLazy;
		ImageLoader::InitializerRunning runInitializers = ImageLoader::kRunInitializers;
		if ( (options & NSLINKMODULE_OPTION_DONT_CALL_MOD_INIT_ROUTINES) != 0 )
			runInitializers = ImageLoader::kDontRunInitializersButTellObjc;
		
		// load libraries, rebase, bind, to make this image usable
		dyld::link(objectFileImage->image, bindness, runInitializers);
		
		return ImageLoaderToNSModule(objectFileImage->image);
	}
	catch (const char* msg) {
		if ( (options & NSLINKMODULE_OPTION_RETURN_ON_ERROR) == 0 )
			dyldAPIhalt(__func__, msg);
		// not halting, so set error state for NSLinkEditError to find
		dyld::removeImage(objectFileImage->image);
		setLastError(NSLinkEditOtherError, 0, moduleName, msg);
		return NULL;
	}
}

#if OLD_LIBSYSTEM_SUPPORT
// This is for compatibility with old libSystems (libdyld.a) which process ObjectFileImages outside dyld
static NSModule _dyld_link_module(NSObjectFileImage object_addr, size_t object_size, const char* moduleName, uint32_t options)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, \"%s\", 0x%08X)\n", "NSLinkModule", object_addr,  moduleName, options); // note name/args translation
	ImageLoader* image = NULL;
	dyld::clearErrorMessage();
	try {
		const char* imageName = moduleName;
		if ( (options & NSLINKMODULE_OPTION_TRAILING_PHYS_NAME) != 0 )
			imageName = &moduleName[strlen(moduleName)+1];
			
		image = dyld::loadFromMemory((const uint8_t*)object_addr, object_size, imageName); 
		
		if ( (options & NSLINKMODULE_OPTION_TRAILING_PHYS_NAME) != 0 )
			image->setLogicalPath(moduleName);
			
		if ( image != NULL ) {		
			// support private bundles
			if ( (options & NSLINKMODULE_OPTION_PRIVATE) != 0 )
				image->setHideExports();
		
			// set up linking options
			ImageLoader::BindingLaziness bindness = ImageLoader::kNonLazyOnly;
			if ( (options & NSLINKMODULE_OPTION_BINDNOW) != 0 )
				bindness = ImageLoader::kLazyAndNonLazy;
			ImageLoader::InitializerRunning runInitializers = ImageLoader::kRunInitializers;
			if ( (options & NSLINKMODULE_OPTION_DONT_CALL_MOD_INIT_ROUTINES) != 0 )
				runInitializers = ImageLoader::kDontRunInitializersButTellObjc;
			
			// load libraries, rebase, bind, to make this image usable
			dyld::link(image, bindness, runInitializers);
		}
	}
	catch (const char* msg) {
		if ( (options & NSLINKMODULE_OPTION_RETURN_ON_ERROR) == 0 )
			dyldAPIhalt("NSLinkModule", msg);
		// not halting, so set error state for NSLinkEditError to find
		setLastError(NSLinkEditOtherError, 0, moduleName, msg);
		// if image was created for this bundle, destroy it
		if ( image != NULL ) {
			dyld::removeImage(image);
			delete image;
		}
		image = NULL;
	}
	return ImageLoaderToNSModule(image);
}
#endif

NSSymbol NSLookupSymbolInModule(NSModule module, const char* symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, \"%s\")\n", __func__, (void *)module, symbolName);
	ImageLoader* image = NSModuleToImageLoader(module);
	if ( image == NULL ) 
		return NULL;
	return SymbolToNSSymbol(image->findExportedSymbol(symbolName, NULL, false, NULL));
}

const char* NSNameOfModule(NSModule module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, module);
	ImageLoader* image = NSModuleToImageLoader(module);
	if ( image == NULL ) 
		return NULL;
	return image->getPath();
}

const char* NSLibraryNameForModule(NSModule module)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, module);
	ImageLoader* image = NSModuleToImageLoader(module);
	if ( image == NULL ) 
		return NULL;
	return image->getPath();
}

bool NSUnLinkModule(NSModule module, uint32_t options)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, 0x%08X)\n", __func__, module, options); 
	if ( module == NULL )
		return false;
	ImageLoader* image = NSModuleToImageLoader(module);
	if ( image == NULL ) 
		return false;
	dyld::removeImage(image);
	
	if ( (options & NSUNLINKMODULE_OPTION_KEEP_MEMORY_MAPPED) != 0 )
		image->setLeaveMapped();
		
	// TODO: NSUNLINKMODULE_OPTION_RESET_LAZY_REFERENCES

	// Only delete image if there is no ofi referencing it
	// That means the ofi was destroyed after linking, so no one is left to delete this image	
	const int ofiCount = sObjectFileImages.size();
	bool found = false;
	for (int i=0; i < ofiCount; ++i) {
		NSObjectFileImage ofi = sObjectFileImages[i];
		if ( ofi->image == image )
			found = true;
	}
	if ( !found )
		delete image;
	
	return true;
}

// internal name and parameters do not match public name and parameters...
static void _dyld_install_handlers(void* undefined, void* multiple, void* linkEdit)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "NSLinkEditErrorHandlers()\n");

	dyld::registerUndefinedHandler((dyld::UndefinedHandler)undefined);
	// no support for multiple or linkedit handlers
}

const struct mach_header * _dyld_get_image_header_containing_address(const void* address)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, address);
	ImageLoader* image = dyld::findImageContainingAddress(address);
	if ( image != NULL ) 
		return image->machHeader();
	return NULL;
}


void _dyld_register_func_for_add_image(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, (void *)func);
	dyld::registerAddCallback(func);
}

void _dyld_register_func_for_remove_image(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, (void *)func);
	dyld::registerRemoveCallback(func);
}

// called by atexit() function installed by crt
static void _dyld_mod_term_funcs()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	dyld::runTerminators();
}

// called by crt before main
static void _dyld_make_delayed_module_initializer_calls()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	dyld::initializeMainExecutable();
}


void NSLinkEditError(NSLinkEditErrors* c, int* errorNumber, const char** fileName, const char** errorString)
{
	// FIXME FIXME
	*c = sLastErrorFileCode;
	*errorNumber = sLastErrorNo;
	*fileName = sLastErrorFilePath;
	*errorString = dyld::getErrorMessage();
}

static void _dyld_register_binding_handler(void * (*bindingHandler)(const char *, const char *, void *), ImageLoader::BindingOptions bindingOptions)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	dyld::gLinkContext.bindingHandler = bindingHandler;
	dyld::gLinkContext.bindingOptions = bindingOptions;
}

// Call by fork() in libSystem before the kernel trap is done
static void _dyld_fork_prepare()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
}

// Call by fork() in libSystem after the kernel trap is done on the parent side
static void _dyld_fork_parent()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
}

// Call by fork() in libSystem after the kernel trap is done on the child side
static void _dyld_fork_child()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
	// The implementation of fork() in libSystem knows to reset the variable mach_task_self_
	// in libSystem for the child of a fork.  But dyld is built with a static copy
	// of libc.a and has its own copy of mach_task_self_ which we reset here.
	//
	// In mach_init.h mach_task_self() is #defined to mach_task_self_ and
	// in mach_init() mach_task_self_ is initialized to task_self_trap().
	//
	extern mach_port_t	mach_task_self_;
	mach_task_self_ = task_self_trap();
}

// Call by fork() in libSystem after the kernel trap is done on the child side after
// other libSystem child side fixups are done
static void _dyld_fork_child_final()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
}


typedef void (*MonitorProc)(char *lowpc, char *highpc);

static void monInitCallback(ImageLoader* image, void* userData)
{
	MonitorProc proc = (MonitorProc)userData;
	void* start;
	size_t length;
	if ( image->getSectionContent("__TEXT", "__text", &start, &length) ) {
		proc((char*)start, (char*)start+length);
	}
}

//
// _dyld_moninit is called from profiling runtime routine moninit().
// dyld calls back with the range of each __TEXT/__text section in every
// linked image.
//
void _dyld_moninit(MonitorProc proc)
{
	dyld::forEachImageDo(&monInitCallback, (void*)proc);
}

// returns true if prebinding was used in main executable
bool _dyld_launched_prebound()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);
		
	// ¥¥¥Êif we deprecate prebinding, we may want to consider always returning true or false here
	return dyld::mainExecutablePrebound();
}


//
// _dyld_NSMakePrivateModulePublic() is the dyld side of the hack
// NSMakePrivateModulePublic() needed for the dlopen() to turn it's
// RTLD_LOCAL handles into RTLD_GLOBAL.  It just simply turns off the private
// flag on the image for this module.  If the module was found and it was
// private then everything worked and TRUE is returned else FALSE is returned.
//
static bool NSMakePrivateModulePublic(NSModule module)
{
	ImageLoader* image = NSModuleToImageLoader(module);
	if ( image != NULL ) {
		if ( image->hasHiddenExports() ) {
			image->setHideExports(false);
			return true;
		}
	}
	return false;
}



bool lookupDyldFunction(const char* name, uintptr_t* address)
{
	for (const dyld_func* p = dyld_funcs; p->name != NULL; ++p) {
	    if ( strcmp(p->name, name) == 0 ) {
			if( p->implementation == unimplemented )
				fprintf(stderr, "unimplemented dyld function: %s\n", p->name);
			*address = (uintptr_t)p->implementation;
			return true;
	    }
	}
	*address = 0;
	return false;
}


static void registerThreadHelpers(const dyld::ThreadingHelpers* helpers)
{
	// We need to make sure libSystem's lazy pointer's are bound
	// before installing thred helpers.
	// The reason for this is that if accessing the lock requires
	// a lazy pointer to be bound (and it does when multi-module
	// libSystem) is not prebound, the lazy handler will be
	// invoked which tries to acquire the lock again...an infinite 
	// loop.
	ImageLoader* image = dyld::findImageContainingAddress(helpers);
	dyld::link(image, ImageLoader::kLazyOnly, ImageLoader::kDontRunInitializers);

	dyld::gThreadHelpers = helpers;
}


static void dlerrorClear()
{
	if ( dyld::gThreadHelpers != NULL ) {
		char* buffer = (*dyld::gThreadHelpers->getThreadBufferFor_dlerror)(1);
		buffer[0] = '\0';
	}
}

static void dlerrorSet(const char* msg)
{
	if ( dyld::gThreadHelpers != NULL ) {
		char* buffer = (*dyld::gThreadHelpers->getThreadBufferFor_dlerror)(strlen(msg)+1);
		strcpy(buffer, msg);
	}
}



void* dlopen(const char* path, int mode)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%s, 0x%08X)\n", __func__, path, mode);

	dlerrorClear();
	
	// passing NULL for path means return magic object
	if ( path == NULL ) {
		return RTLD_DEFAULT;
	}
	
	try {
		void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
		ImageLoader* callerImage = dyld::findImageContainingAddress(callerAddress);

		ImageLoader*	image = NULL;
		dyld::LoadContext context;
		context.useSearchPaths	= true;
		context.useLdLibraryPath= (strchr(path, '/') == NULL);	// a leafname implies should search 
		context.matchByInstallName = true;
		context.dontLoad = ( (mode & RTLD_NOLOAD) != 0 );
		context.mustBeBundle	= false;
		context.mustBeDylib		= false;
		context.origin			= callerImage != NULL ? callerImage->getPath() : NULL; // caller's image's path
		context.rpath			= NULL; // support not yet implemented
		
		image = load(path, context);
		if ( image != NULL ) {
			image->incrementReferenceCount();
			if ( ! image->isLinked() ) {
				ImageLoader::BindingLaziness bindiness = ImageLoader::kNonLazyOnly;
				if ( (mode & RTLD_NOW) != 0 )
					bindiness = ImageLoader::kLazyAndNonLazy;
				dyld::link(image, bindiness, ImageLoader::kRunInitializers);
				// only hide exports if image is not already in use
				if ( (mode & RTLD_LOCAL) != 0 )
					image->setHideExports(true);
			}
			// RTLD_NODELETE means don't unmap image even after dlclosed. This is what dlcompat did on Mac OS X 10.3
			// On other *nix OS's, it means dlclose() should do nothing, but the handle should be invalidated. 
			// The subtle differences are: 
			//  1) if the image has any termination routines, whether they are run during dlclose or when the process terminates
			//  2) If someone does a supsequent dlopen() on the same image, whether the same address should be used. 
			if ( (mode & RTLD_NODELETE) != 0 )
				image->setLeaveMapped();
			return image;
		}
	}
	catch (const char* msg) {
		const char* format = "dlopen(%s, %d): %s";
		char temp[strlen(format)+strlen(path)+strlen(msg)+10];
		sprintf(temp, format, path, mode, msg);
		dlerrorSet(temp);
	}
	return NULL;
}

int dlclose(void* handle)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p)\n", __func__, handle);

	ImageLoader* image = (ImageLoader*)handle;
	if ( dyld::validImage(image) ) {
		if ( image->decrementReferenceCount() ) {
			// for now, only bundles can be unloaded
			// to unload dylibs we would need to track all direct and indirect uses
			if ( image->isBundle() ) {
				dyld::removeImage(image);
				delete image;
			}
		}
		dlerrorClear();
		return 0;
	}
	else {
		dlerrorSet("invalid handle passed to dlclose()");
		return -1;
	}
}



int dladdr(const void* address, Dl_info* info)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, %p)\n", __func__, address, info);

	ImageLoader* image = dyld::findImageContainingAddress(address);
	if ( image != NULL ) {
		info->dli_fname = image->getLogicalPath();
		info->dli_fbase = (void*)image->machHeader();	
		// find closest exported symbol in the image
		const uint32_t exportCount = image->getExportedSymbolCount();
		const ImageLoader::Symbol* bestSym = NULL;
		const void* bestAddr = 0;
		for(uint32_t i=0; i < exportCount; ++i) {
			const ImageLoader::Symbol* sym = image->getIndexedExportedSymbol(i);
			const void* symAddr = (void*)image->getExportedSymbolAddress(sym);
			if ( (symAddr <= address) && (bestAddr < symAddr) ) {
				bestSym = sym;
				bestAddr = symAddr;
			}
		}
		if ( bestSym != NULL ) {
			info->dli_sname = image->getExportedSymbolName(bestSym) + 1; // strip off leading underscore
			info->dli_saddr = (void*)bestAddr;
		}
		else {
			info->dli_sname = NULL;
			info->dli_saddr = NULL;
		}
		return 1; // success
	}
	return 0;  // failure
}


char* dlerror()
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);

	if ( dyld::gThreadHelpers != NULL ) {
		char* buffer = (*dyld::gThreadHelpers->getThreadBufferFor_dlerror)(1);
		// if no error set, return NULL
		if ( buffer[0] != '\0' )
			return buffer;
	}
	return NULL;
}

void* dlsym(void* handle, const char* symbolName)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s(%p, %s)\n", __func__, handle, symbolName);

	dlerrorClear();

	ImageLoader* image;
	const ImageLoader::Symbol* sym;

	// dlsym() assumes symbolName passed in is same as in C source code
	// dyld assumes all symbol names have an underscore prefix
	char underscoredName[strlen(symbolName)+2];
	underscoredName[0] = '_';
	strcpy(&underscoredName[1], symbolName);
	
	// magic "search all" handle
	if ( handle == RTLD_DEFAULT ) {
		if ( dyld::flatFindExportedSymbol(underscoredName, &sym, &image) ) {
			return (void*)image->getExportedSymbolAddress(sym);
		}
		const char* format = "dlsym(RTLD_DEFAULT, %s): symbol not found";
		char temp[strlen(format)+strlen(symbolName)+2];
		sprintf(temp, format, symbolName);
		dlerrorSet(temp);
		return NULL;
	}
	
	// magic "search what I would see" handle
	if ( handle == RTLD_NEXT ) {
		void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
		ImageLoader* callerImage = dyld::findImageContainingAddress(callerAddress);
		sym = callerImage->findExportedSymbolInDependentImages(underscoredName, &image); // don't search image, but do search what it links against
		if ( sym != NULL ) {
			return (void*)image->getExportedSymbolAddress(sym);
		}
		const char* format = "dlsym(RTLD_NEXT, %s): symbol not found";
		char temp[strlen(format)+strlen(symbolName)+2];
		sprintf(temp, format, symbolName);
		dlerrorSet(temp);
		return NULL;
	}
#ifdef RTLD_SELF
	// magic "search me, then what I would see" handle
	if ( handle == RTLD_SELF ) {
		void* callerAddress = __builtin_return_address(1); // note layers: 1: real client, 0: libSystem glue
		ImageLoader* callerImage = dyld::findImageContainingAddress(callerAddress);
		sym = callerImage->findExportedSymbolInImageOrDependentImages(underscoredName, &image); // search image and what it links against
		if ( sym != NULL ) {
			return (void*)image->getExportedSymbolAddress(sym);
		}
		const char* format = "dlsym(RTLD_SELF, %s): symbol not found";
		char temp[strlen(format)+strlen(symbolName)+2];
		sprintf(temp, format, symbolName);
		dlerrorSet(temp);
		return NULL;
	}
#endif
	// real handle
	image = (ImageLoader*)handle;
	if ( dyld::validImage(image) ) {
		sym = image->findExportedSymbolInImageOrDependentImages(underscoredName, &image); // search image and what it links against
		if ( sym != NULL ) {
			return (void*)image->getExportedSymbolAddress(sym);
		}
		const char* format = "dlsym(%p, %s): symbol not found";
		char temp[strlen(format)+strlen(symbolName)+20];
		sprintf(temp, format, handle, symbolName);
		dlerrorSet(temp);
	}
	else {
		dlerrorSet("invalid handle passed to dlsym()");
	}
	return NULL;
}


static void commitRepreboundFiles(std::vector<ImageLoader*> files, bool unmapOld)
{
	// tell file system to flush all dirty buffers to disk
	// after this sync, the _redoprebinding files will be on disk
	sync();

	// now commit (swap file) for each re-prebound image
	// this only updates directories, since the files have already been flushed by previous sync()
	for (std::vector<ImageLoader*>::iterator it=files.begin(); it != files.end(); it++) {
		(*it)->reprebindCommit(dyld::gLinkContext, true, unmapOld);
	}

	// tell file system to flush all dirty buffers to disk
	// this should flush out all directory changes caused by the file swapping
	sync();
}


#define UPDATE_PREBINDING_DRY_RUN  0x00000001
#define UPDATE_PREBINDING_PROGRESS 0x00000002

//
// SPI called only by update_prebinding tool to redo prebinding in all prebound files specified
// There must be no dylibs loaded when this fnction is called.
//
__attribute__((noreturn))
static void _dyld_update_prebinding(int pathCount, const char* paths[], uint32_t flags)
{
	if ( dyld::gLogAPIs )
		fprintf(stderr, "%s()\n", __func__);

	// list of requested dylibs actually loaded
	std::vector<ImageLoader*> preboundImages;
	
	try {
		// verify no dylibs loaded
		if ( dyld::getImageCount() != 1 )
			throw "_dyld_update_prebinding cannot be called with dylib already loaded";
			
		const uint32_t max_allowed_link_errors = 10;
		uint32_t link_error_count = 0;
		
		// load and link each dylib
		for (int i=0; i < pathCount; ++i) {
			dyld::LoadContext context;
			context.useSearchPaths		= false;
			context.matchByInstallName	= true;
			context.dontLoad			= false;
			context.mustBeBundle		= false;
			context.mustBeDylib			= true;
			context.origin				= NULL;		// @loader_path not allowed in prebinding list
			context.rpath				= NULL;		// support not yet implemented
			
			ImageLoader* image = NULL;
			try {
				image = dyld::load(paths[i], context);
				// bind lazy and non-lazy symbols, but don't run initializers
				// this may bring in other dylibs later in the list or missing from list, but that is ok
				dyld::link(image, ImageLoader::kLazyAndNonLazy, ImageLoader::kDontRunInitializers);
				// recored images we successfully loaded
				preboundImages.push_back(image);
			}
			catch (const char* msg) {
				if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) ) {
					const char *stage;
					if ( image == NULL ) // load exception
						stage = "load";
					else // link exception
						stage = "link";
					fprintf(stderr, "update_prebinding: warning: could not %s %s: %s\n", stage, paths[i], msg);
				}
				if ( image != NULL )
					link_error_count++;
				if ( link_error_count > max_allowed_link_errors )
					throw;
			}
		}
		
		// find missing images
		uint32_t loadedImageCount =	dyld::getImageCount();
		if ( loadedImageCount > (preboundImages.size()+1) ) {
			if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) )
				fprintf(stderr, "update_prebinding: warning: the following dylibs were loaded but will not have their prebinding updated because they are not in the list of paths to reprebind\n");
			for (uint32_t i=1; i < loadedImageCount; ++i) {
				ImageLoader* target = dyld::getIndexedImage(i);
				bool found = false;
				for (std::vector<ImageLoader*>::iterator it=preboundImages.begin(); it != preboundImages.end(); it++) {
					if ( *it == target ) {
						found = true;
						break;
					}
				}
				if ( !found ) 
					if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) )
						fprintf(stderr, "  %s\n", target->getPath());
			}
		}

		// warn about unprebound files in the list
		bool unpreboundWarned = false;
		for (std::vector<ImageLoader*>::iterator it=preboundImages.begin(); it != preboundImages.end(); it++) {
			if ( ! (*it)->isPrebindable() && (*it != dyld::mainExecutable()) ) {
				if ( ! unpreboundWarned ) {
					if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) )
						fprintf(stderr, "update_prebinding: warning: the following dylibs were specified but were not built prebound\n");
					unpreboundWarned = true;
				}
				if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) )
					fprintf(stderr, "  %s\n", (*it)->getPath());
			}
		}
		
		if(UPDATE_PREBINDING_DRY_RUN & flags) {
			fprintf(stderr, "update_prebinding: dry-run: no changes were made to the filesystem.\n");
		}
		else {
			uint32_t imageCount = preboundImages.size();
			uint32_t imageNumber = 1;

			// on Intel system, update_prebinding is run twice: i386, then emulated ppc
			// calculate fudge factors so that progress output represents both runs
			int denomFactor = 1;
			int numerAddend = 0;
			if (UPDATE_PREBINDING_PROGRESS & flags) {
		#if __i386__
				// i386 half runs first, just double denominator
				denomFactor = 2;
		#endif	
		#if __ppc__
				// if emulated ppc, double denominator and shift numerator
				int mib[] = { CTL_KERN, KERN_CLASSIC, getpid() };
				int is_emulated = 0;
				size_t len = sizeof(int);
				int ret = sysctl(mib, 3, &is_emulated, &len, NULL, 0);
				if ((ret != -1) && is_emulated) {
					denomFactor = 2;
					numerAddend = imageCount;
				}
		#endif
			}

			// tell each image to write itself out re-prebound
			struct timeval currentTime = { 0 , 0 };
			gettimeofday(&currentTime, NULL);
			time_t timestamp = currentTime.tv_sec;
			std::vector<ImageLoader*> updatedImages;
			for (std::vector<ImageLoader*>::iterator it=preboundImages.begin(); it != preboundImages.end(); it++) {
				uint64_t freespace = (*it)->reprebind(dyld::gLinkContext, timestamp);
				updatedImages.push_back(*it);
				if(UPDATE_PREBINDING_PROGRESS & flags) {
					fprintf(stdout, "update_prebinding: progress: %3u/%u\n", imageNumber+numerAddend, imageCount*denomFactor);
					fflush(stdout);
					imageNumber++;
				}
				// see if we are running low on disk space (less than 32MB is "low")
				const uint64_t kMinFreeSpace = 32*1024*1024;  
				if ( freespace < kMinFreeSpace ) {
					if ( dyld::gLinkContext.verbosePrebinding || (UPDATE_PREBINDING_DRY_RUN & flags) )
						fprintf(stderr, "update_prebinding: disk space down to %lluMB, committing %lu prebound files\n", freespace/(1024*1024), updatedImages.size());
					// commit files processed so far, to free up more disk space
					commitRepreboundFiles(updatedImages, true);
					// empty list of temp files
					updatedImages.clear();
				}
			}
		
			// commit them, don't need to unmap old, cause we are done
			commitRepreboundFiles(updatedImages, false);
		}
	}
	catch (const char* msg) {
		// delete temp files
		try {
			for (std::vector<ImageLoader*>::iterator it=preboundImages.begin(); it != preboundImages.end(); it++) {
				(*it)->reprebindCommit(dyld::gLinkContext, false, false);
			}
		}
		catch (const char* commitMsg) {
			fprintf(stderr, "update_prebinding: error: %s\n", commitMsg);
		}
		fprintf(stderr, "update_prebinding: error: %s\n", msg);
		exit(1);
	}
	exit(0);
}



static const struct dyld_all_image_infos* _dyld_get_all_image_infos()
{
	return &dyld_all_image_infos;
}








