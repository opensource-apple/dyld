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

template <typename A>
class objc_method_t {
    typename A::P::uint_t name;   // SEL
    typename A::P::uint_t types;  // const char *
    typename A::P::uint_t imp;    // IMP

public:
    typename A::P::uint_t getName() const { return A::P::getP(name); }
    void setName(typename A::P::uint_t newName) { A::P::setP(name, newName); }
};

template <typename A>
class objc_method_list_t {
    uint32_t entsize;
    uint32_t count;
    objc_method_t<A> first;

	uint32_t getEntsize() const { return A::P::E::get32(entsize) & ~(uint32_t)3; }

public:
    typename A::P::uint_t getCount() const { return A::P::E::get32(count); }

    objc_method_t<A>& get(typename A::P::uint_t i) const { return *(objc_method_t<A> *)((uint8_t *)&first + i * getEntsize()); }

    void setFixedUp() { A::P::E::set32(entsize, getEntsize() | 3); }
};

template <typename A>
class objc_ivar_t {
    typename A::P::uint_t offset;  // A::P *
    typename A::P::uint_t name;    // const char *
    typename A::P::uint_t type;    // const char *
    uint32_t alignment; 
    uint32_t size;
};

template <typename A>
class objc_ivar_list_t {
    uint32_t entsize;
    uint32_t count;
    objc_ivar_t<A> first;

public:
    objc_ivar_t<A>& getIvarAtIndex(typename A::P::pint_t i) const { return *(objc_ivar_t<A> *)((uint8_t *)&first + i * A::P::E::get32(entsize)); }
};

template <typename A>
class objc_protocol_t {
    typename A::P::uint_t isa;
    typename A::P::uint_t name;
    typename A::P::uint_t protocols;
    typename A::P::uint_t instanceMethods;
    typename A::P::uint_t classMethods;
    typename A::P::uint_t optionalInstanceMethods;
    typename A::P::uint_t optionalClassMethods;
    typename A::P::uint_t instanceProperties;

public:
    objc_method_list_t<A> *getInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(instanceMethods)); }

    objc_method_list_t<A> *getClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(classMethods)); }

    objc_method_list_t<A> *getOptionalInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(optionalInstanceMethods)); }

    objc_method_list_t<A> *getOptionalClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(optionalClassMethods)); }

};

template <typename A>
class objc_protocol_list_t {
    typename A::P::uint_t count;
    typename A::P::uint_t list[0];
};

template < typename P, typename E > 
struct pad { };

template < typename E >
struct pad< Pointer64<E>, E > { uint32_t unused; };

template <typename A>
class objc_class_data_t {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    pad<typename A::P, typename A::P::E> reserved;  // ILP32=0 bytes, LP64=4 bytes

    typename A::P::uint_t ivarLayout;
    typename A::P::uint_t name;
    typename A::P::uint_t baseMethods;
    typename A::P::uint_t baseProtocols;
    typename A::P::uint_t ivars;
    typename A::P::uint_t weakIvarLayout;
    typename A::P::uint_t baseProperties;

public:
    objc_method_list_t<A> *getMethodList(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(baseMethods)); }
};

template <typename A>
class objc_class_t {
    typename A::P::uint_t isa;
    typename A::P::uint_t superclass;
    typename A::P::uint_t method_cache;
    typename A::P::uint_t vtable;
    typename A::P::uint_t data;

public:
    objc_class_t<A> *getIsa(SharedCache<A> *cache) const { return (objc_class_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(isa)); }

    objc_class_data_t<A> *getData(SharedCache<A>* cache) const { return (objc_class_data_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(data)); }

    objc_method_list_t<A> *getMethodList(SharedCache<A>* cache) const { return getData(cache)->getMethodList(cache); }
};



template <typename A>
class objc_category_t {
    typename A::P::uint_t name;
    typename A::P::uint_t cls;
    typename A::P::uint_t instanceMethods;
    typename A::P::uint_t classMethods;
    typename A::P::uint_t protocols;
    typename A::P::uint_t instanceProperties;

public:
    objc_method_list_t<A> *getInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(instanceMethods)); }

    objc_method_list_t<A> *getClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedCacheAddressForAddress(A::P::getP(classMethods)); }
};

template <typename A>
class objc_message_ref_t {
    typename A::P::uint_t imp;
    typename A::P::uint_t sel;

public:
    typename A::P::uint_t getName() const { return A::P::getP(sel); }

    void setName(typename A::P::uint_t newName) { A::P::setP(sel, newName); }
};

template <typename A, typename V>
class SelectorUpdater {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    static void visitMethodList(objc_method_list_t<A> *mlist, V& visitor)
    {
        for (pint_t m = 0; m < mlist->getCount(); m++) {
            pint_t oldValue = mlist->get(m).getName();
            pint_t newValue = visitor.visit(oldValue);
            mlist->get(m).setName(newValue);
        }
        mlist->setFixedUp();
    }

public:

    static void update(SharedCache<A>* cache, const macho_header<P>* header, 
                       V& visitor)
    {
        // Method lists in classes
        PointerSection<A, objc_class_t<A> *> 
            classes(cache, header, "__DATA", "__objc_classlist");
        for (pint_t i = 0; i < classes.count(); i++) {
            objc_class_t<A> *cls = classes.get(i);
            objc_method_list_t<A> *mlist;
            if ((mlist = cls->getMethodList(cache))) {
                visitMethodList(mlist, visitor);
            }
            if ((mlist = cls->getIsa(cache)->getMethodList(cache))) {
                visitMethodList(mlist, visitor);
            }
        }
        
        // Method lists from categories
        PointerSection<A, objc_category_t<A> *> 
            cats(cache, header, "__DATA", "__objc_catlist");
        for (pint_t i = 0; i < cats.count(); i++) {
            objc_category_t<A> *cat = cats.get(i);
            objc_method_list_t<A> *mlist;
            if ((mlist = cat->getInstanceMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
            if ((mlist = cat->getClassMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
        }

        // Method description lists from protocols
        PointerSection<A, objc_protocol_t<A> *> 
            protocols(cache, header, "__DATA", "__objc_protolist");
        for (pint_t i = 0; i < protocols.count(); i++) {
            objc_protocol_t<A> *proto = protocols.get(i);
            objc_method_list_t<A> *mlist;
            if ((mlist = proto->getInstanceMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
            if ((mlist = proto->getClassMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
            if ((mlist = proto->getOptionalInstanceMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
            if ((mlist = proto->getOptionalClassMethods(cache))) {
                visitMethodList(mlist, visitor);
            }
        }

        // @selector references
        PointerSection<A, const char *> 
            selrefs(cache, header, "__DATA", "__objc_selrefs");
        for (pint_t i = 0; i < selrefs.count(); i++) {
            pint_t oldValue = selrefs.getUnmapped(i);
            pint_t newValue = visitor.visit(oldValue);
            selrefs.set(i, newValue);
        }

        // message references
        ArraySection<A, objc_message_ref_t<A> > 
            msgrefs(cache, header, "__DATA", "__objc_msgrefs");
        for (pint_t i = 0; i < msgrefs.count(); i++) {
            objc_message_ref_t<A>& msg = msgrefs.get(i);
            pint_t oldValue = msg.getName();
            pint_t newValue = visitor.visit(oldValue);
            msg.setName(newValue);
        }

        // Mark image_info
        const macho_section<P> *imageInfoSection = 
            header->getSection("__DATA", "__objc_imageinfo");
        if (imageInfoSection) {
            objc_image_info<A> *info = (objc_image_info<A> *)
                cache->mappedCacheAddressForAddress(imageInfoSection->addr());
            info->setSelectorsPrebound();
        }
    }
};
