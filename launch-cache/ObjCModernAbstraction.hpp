/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2008-2010 Apple Inc. All rights reserved.
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

#include "MachOLayout.hpp"
#include <iterator>
#include <deque>

// iterate an entsize-based list
// typedef entsize_iterator< A, type_t<A>, type_list_t<A> > type_iterator;
template <typename A, typename T, typename Tlist>
struct entsize_iterator {
    uint32_t entsize;
    uint32_t index;  // keeping track of this saves a divide in operator-
    T* current;    

    typedef std::random_access_iterator_tag iterator_category;
    typedef T value_type;
    typedef ptrdiff_t difference_type;
    typedef T* pointer;
    typedef T& reference;
    
    entsize_iterator() { } 
    
    entsize_iterator(const Tlist& list, uint32_t start = 0)
        : entsize(list.getEntsize()), index(start), current(&list.get(start)) 
    { }
    
    const entsize_iterator<A,T,Tlist>& operator += (ptrdiff_t count) {
        current = (T*)((uint8_t *)current + count*entsize);
        index += count;
        return *this;
    }
    const entsize_iterator<A,T,Tlist>& operator -= (ptrdiff_t count) {
        current = (T*)((uint8_t *)current - count*entsize);
        index -= count;
        return *this;
    }
    const entsize_iterator<A,T,Tlist> operator + (ptrdiff_t count) const {
        return entsize_iterator(*this) += count;
    }
    const entsize_iterator<A,T,Tlist> operator - (ptrdiff_t count) const {
        return entsize_iterator(*this) -= count;
    }
    
    entsize_iterator<A,T,Tlist>& operator ++ () { *this += 1; return *this; }
    entsize_iterator<A,T,Tlist>& operator -- () { *this -= 1; return *this; }
    entsize_iterator<A,T,Tlist> operator ++ (int) { 
        entsize_iterator<A,T,Tlist> result(*this); *this += 1; return result; 
    }
    entsize_iterator<A,T,Tlist> operator -- (int) { 
        entsize_iterator<A,T,Tlist> result(*this); *this -= 1; return result; 
    }
    
    ptrdiff_t operator - (const entsize_iterator<A,T,Tlist>& rhs) const {
        return (ptrdiff_t)this->index - (ptrdiff_t)rhs.index;
    }
    
    T& operator * () { return *current; }
    T& operator * () const { return *current; }
    T& operator -> () { return *current; }
    const T& operator -> () const { return *current; }
    
    operator T& () const { return *current; }
    
    bool operator == (const entsize_iterator<A,T,Tlist>& rhs) {
        return this->current == rhs.current;
    }
    bool operator != (const entsize_iterator<A,T,Tlist>& rhs) {
        return this->current != rhs.current;
    }
    
    bool operator < (const entsize_iterator<A,T,Tlist>& rhs) {
        return this->current < rhs.current;
    }        
    bool operator > (const entsize_iterator<A,T,Tlist>& rhs) {
        return this->current > rhs.current;
    }

    
    static void overwrite(entsize_iterator<A,T,Tlist>& dst, const Tlist* srcList)
    {
        entsize_iterator<A,T,Tlist> src;
        uint32_t ee = srcList->getEntsize();
        for (src = srcList->begin(); src != srcList->end(); ++src) {
            memcpy(&*dst, &*src, ee);
            ++dst;
        }
    }
};

template <typename A> 
class objc_header_info_t {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    pint_t next;   // objc_header_info *
    pint_t mhdr;   // mach_header or mach_header_64
    pint_t info;   // objc_image_info *
    pint_t fname;  // const char *
    bool loaded; 
    bool inSharedCache;
    bool allClassesRealized;

public:
    objc_header_info_t(SharedCache<A>* cache, const macho_header<P>* mh) 
        : next(0), 
          mhdr(0), 
          info(0), 
          fname(0), 
          loaded(0), 
          allClassesRealized(0)
    {
        A::P::setP(mhdr, cache->VMAddressForMappedAddress(mh));
        const macho_section<P>* sect = mh->getSection("__DATA", "__objc_imageinfo");
        if (sect) A::P::setP(info, sect->addr());

        // can't set fname because dyld sometimes edits it
    }

	void addPointers(std::vector<void*>& pointersToAdd) {
        pointersToAdd.push_back(&mhdr);
        if (info) pointersToAdd.push_back(&info);
    }

    uint64_t header_vmaddr() const { return mhdr; }
};
  
template <typename A> class objc_method_list_t;  // forward reference

template <typename A>
class objc_method_t {
    typename A::P::uint_t name;   // SEL
    typename A::P::uint_t types;  // const char *
    typename A::P::uint_t imp;    // IMP
	friend class objc_method_list_t<A>;
public:
    typename A::P::uint_t getName() const { return A::P::getP(name); }
    void setName(typename A::P::uint_t newName) { A::P::setP(name, newName); }

    struct SortBySELAddress : 
        public std::binary_function<const objc_method_t<A>&, 
                                    const objc_method_t<A>&, bool>
    {
        bool operator() (const objc_method_t<A>& lhs, 
                         const objc_method_t<A>& rhs)
        {
            return lhs.getName() < rhs.getName();
        }
    };
};

template <typename A>
class objc_method_list_t {
    uint32_t entsize;
    uint32_t count;
    objc_method_t<A> first;

    // use newMethodList instead
    void* operator new (size_t) { return NULL; }
    void* operator new (size_t, void* buf) { return buf; }

public:

    typedef entsize_iterator< A, objc_method_t<A>, objc_method_list_t<A> > method_iterator;

    uint32_t getCount() const { return A::P::E::get32(count); }

	uint32_t getEntsize() const {return A::P::E::get32(entsize)&~(uint32_t)3;}

    objc_method_t<A>& get(uint32_t i) const { return *(objc_method_t<A> *)((uint8_t *)&first + i * getEntsize()); }

    uint32_t byteSize() const { 
        return byteSizeForCount(getCount(), getEntsize()); 
    }

    static uint32_t byteSizeForCount(uint32_t c, uint32_t e = sizeof(objc_method_t<A>)) { 
        return sizeof(objc_method_list_t<A>) - sizeof(objc_method_t<A>) + c*e;
    }

    method_iterator begin() { return method_iterator(*this, 0); }
    method_iterator end() { return method_iterator(*this, getCount()); }
    const method_iterator begin() const { return method_iterator(*this, 0); }
    const method_iterator end() const { return method_iterator(*this, getCount()); }

    void setFixedUp() { A::P::E::set32(entsize, getEntsize() | 3); }

	void getPointers(std::set<void*>& pointersToRemove) {
		for(method_iterator it = begin(); it != end(); ++it) {
			objc_method_t<A>& entry = *it;
			pointersToRemove.insert(&(entry.name));
			pointersToRemove.insert(&(entry.types));
			pointersToRemove.insert(&(entry.imp));
		}
	}
	
	static void addPointers(uint8_t* methodList, std::vector<void*>& pointersToAdd) {
		objc_method_list_t<A>* mlist = (objc_method_list_t<A>*)methodList;
		for(method_iterator it = mlist->begin(); it != mlist->end(); ++it) {
			objc_method_t<A>& entry = *it;
			pointersToAdd.push_back(&(entry.name));
			pointersToAdd.push_back(&(entry.types));
			pointersToAdd.push_back(&(entry.imp));
		}
	}

    static objc_method_list_t<A>* newMethodList(size_t newCount, uint32_t newEntsize) {
        void *buf = ::calloc(byteSizeForCount(newCount, newEntsize), 1);
        return new (buf) objc_method_list_t<A>(newCount, newEntsize);
    }

    void operator delete(void * p) { 
        ::free(p); 
    }

    objc_method_list_t(uint32_t newCount, 
                       uint32_t newEntsize = sizeof(objc_method_t<A>))
        : entsize(newEntsize), count(newCount) 
    { }
};

template <typename A>
class objc_ivar_t {
    typedef typename A::P::uint_t pint_t;
    typename A::P::uint_t offset;  // A::P *
    typename A::P::uint_t name;    // const char *
    typename A::P::uint_t type;    // const char *
    uint32_t alignment; 
    uint32_t size;
    
public:
    const char * getName(SharedCache<A> *cache) const { return (const char *)cache->mappedAddressForVMAddress(A::P::getP(name)); }

    bool hasOffset() const { return A::P::getP(offset) != 0; }
    pint_t getOffset(SharedCache<A> *cache) const { return A::P::getP(*(pint_t * const)cache->mappedAddressForVMAddress(A::P::getP(offset))); }
    void setOffset(SharedCache<A> *cache, pint_t newOffset) { A::P::setP(*(pint_t *)cache->mappedAddressForVMAddress(A::P::getP(offset)), newOffset); }
    
    uint32_t getAlignment()
    {
        uint32_t a = A::P::E::get32(alignment);
        return a == (uint32_t)-1 ? sizeof(typename A::P::uint_t) : 1<<a;
    }
};

template <typename A>
class objc_ivar_list_t {
    typedef typename A::P::uint_t pint_t;
    uint32_t entsize;
    uint32_t count;
    objc_ivar_t<A> first;

    // use newIvarList instead
    void* operator new (size_t) { return NULL; }
    void* operator new (size_t, void* buf) { return buf; }

public:

    typedef entsize_iterator< A, objc_ivar_t<A>, objc_ivar_list_t<A> > ivar_iterator;

    uint32_t getCount() const { return A::P::E::get32(count); }

	uint32_t getEntsize() const { return A::P::E::get32(entsize); }

    objc_ivar_t<A>& get(pint_t i) const { return *(objc_ivar_t<A> *)((uint8_t *)&first + i * A::P::E::get32(entsize)); }

    uint32_t byteSize() const { 
        return byteSizeForCount(getCount(), getEntsize()); 
    }

    static uint32_t byteSizeForCount(uint32_t c, uint32_t e = sizeof(objc_ivar_t<A>)) { 
        return sizeof(objc_ivar_list_t<A>) - sizeof(objc_ivar_t<A>) + c*e;
    }

    ivar_iterator begin() { return ivar_iterator(*this, 0); }
    ivar_iterator end() { return ivar_iterator(*this, getCount()); }
    const ivar_iterator begin() const { return ivar_iterator(*this, 0); }
    const ivar_iterator end() const { return ivar_iterator(*this, getCount()); }

    static objc_ivar_list_t<A>* newIvarList(size_t newCount, uint32_t newEntsize) {
        void *buf = ::calloc(byteSizeForCount(newCount, newEntsize), 1);
        return new (buf) objc_ivar_list_t<A>(newCount, newEntsize);
    }

    void operator delete(void * p) { 
        ::free(p); 
    }

    objc_ivar_list_t(uint32_t newCount, 
                         uint32_t newEntsize = sizeof(objc_ivar_t<A>))
        : entsize(newEntsize), count(newCount) 
    { }

};


template <typename A> class objc_property_list_t; // forward 

template <typename A>
class objc_property_t {
    typename A::P::uint_t name;
    typename A::P::uint_t attributes;
	friend class objc_property_list_t<A>;
public:
    
    const char * getName(SharedCache<A>* cache) const { return (const char *)cache->mappedAddressForVMAddress(A::P::getP(name)); }

    const char * getAttributes(SharedCache<A>* cache) const { return (const char *)cache->mappedAddressForVMAddress(A::P::getP(attributes)); }
};

template <typename A>
class objc_property_list_t {
    uint32_t entsize;
    uint32_t count;
    objc_property_t<A> first;

    // use newPropertyList instead
    void* operator new (size_t) { return NULL; }
    void* operator new (size_t, void* buf) { return buf; }

public:

    typedef entsize_iterator< A, objc_property_t<A>, objc_property_list_t<A> > property_iterator;

    uint32_t getCount() const { return A::P::E::get32(count); }

	uint32_t getEntsize() const { return A::P::E::get32(entsize); }

    objc_property_t<A>& get(uint32_t i) const { return *(objc_property_t<A> *)((uint8_t *)&first + i * getEntsize()); }

    uint32_t byteSize() const { 
        return byteSizeForCount(getCount(), getEntsize()); 
    }

    static uint32_t byteSizeForCount(uint32_t c, uint32_t e = sizeof(objc_property_t<A>)) { 
        return sizeof(objc_property_list_t<A>) - sizeof(objc_property_t<A>) + c*e;
    }

    property_iterator begin() { return property_iterator(*this, 0); }
    property_iterator end() { return property_iterator(*this, getCount()); }
    const property_iterator begin() const { return property_iterator(*this, 0); }
    const property_iterator end() const { return property_iterator(*this, getCount()); }

	void getPointers(std::set<void*>& pointersToRemove) {
		for(property_iterator it = begin(); it != end(); ++it) {
			objc_property_t<A>& entry = *it;
			pointersToRemove.insert(&(entry.name));
			pointersToRemove.insert(&(entry.attributes));
		}
	}

	static void addPointers(uint8_t* propertyList, std::vector<void*>& pointersToAdd) {
		objc_property_list_t<A>* plist = (objc_property_list_t<A>*)propertyList;
		for(property_iterator it = plist->begin(); it != plist->end(); ++it) {
			objc_property_t<A>& entry = *it;
			pointersToAdd.push_back(&(entry.name));
			pointersToAdd.push_back(&(entry.attributes));
		}
	}

     static objc_property_list_t<A>* newPropertyList(size_t newCount, uint32_t newEntsize) {
        void *buf = ::calloc(byteSizeForCount(newCount, newEntsize), 1);
        return new (buf) objc_property_list_t<A>(newCount, newEntsize);
    }

    void operator delete(void * p) { 
        ::free(p); 
    }

    objc_property_list_t(uint32_t newCount, 
                         uint32_t newEntsize = sizeof(objc_property_t<A>))
        : entsize(newEntsize), count(newCount) 
    { }

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
    objc_method_list_t<A> *getInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(instanceMethods)); }

    objc_method_list_t<A> *getClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(classMethods)); }

    objc_method_list_t<A> *getOptionalInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(optionalInstanceMethods)); }

    objc_method_list_t<A> *getOptionalClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(optionalClassMethods)); }

};

template <typename A>
class objc_protocol_list_t {
    typedef typename A::P::uint_t pint_t;
    pint_t count;
    pint_t list[0];

    // use newProtocolList instead
    void* operator new (size_t) { return NULL; }
    void* operator new (size_t, void* buf) { return buf; }

public:

    pint_t getCount() const { return A::P::getP(count); }

    objc_protocol_t<A>* get(SharedCache<A>* cache, pint_t i) {
        return (objc_protocol_t<A>*)cache->mappedAddressForVMAddress(A::P::getP(list[i]));
    }
    
    void overwrite(pint_t& index, const objc_protocol_list_t<A>* src) {
        pint_t srcCount = src->getCount();
        memcpy(list+index, src->list, srcCount * sizeof(pint_t));
        index += srcCount;
    }

    uint32_t byteSize() const { 
        return byteSizeForCount(getCount()); 
    }
    static uint32_t byteSizeForCount(pint_t c) { 
        return sizeof(objc_protocol_list_t<A>) + c*sizeof(pint_t);
    }

	void getPointers(std::set<void*>& pointersToRemove) {
		for(int i=0 ; i < count; ++i) {
			pointersToRemove.insert(&list[i]);
		}
	}

 	static void addPointers(uint8_t* protocolList, std::vector<void*>& pointersToAdd) {
		objc_protocol_list_t<A>* plist = (objc_protocol_list_t<A>*)protocolList;
		for(int i=0 ; i < plist->count; ++i) {
			pointersToAdd.push_back(&plist->list[i]);
		}
	}

    static objc_protocol_list_t<A>* newProtocolList(pint_t newCount) {
        void *buf = ::calloc(byteSizeForCount(newCount), 1);
        return new (buf) objc_protocol_list_t<A>(newCount);
    }

    void operator delete(void * p) { 
        ::free(p); 
    }

    objc_protocol_list_t(uint32_t newCount) : count(newCount) { }

};


template <typename A>
class objc_class_data_t {
    uint32_t flags;
    uint32_t instanceStart;
    // Note there is 4-bytes of alignment padding between instanceSize and ivarLayout
    // on 64-bit archs, but no padding on 32-bit archs.
    // This union is a way to model that.
    union {
        uint32_t                instanceSize;
        typename A::P::uint_t   pad;
    } instanceSize;
    typename A::P::uint_t ivarLayout;
    typename A::P::uint_t name;
    typename A::P::uint_t baseMethods;
    typename A::P::uint_t baseProtocols;
    typename A::P::uint_t ivars;
    typename A::P::uint_t weakIvarLayout;
    typename A::P::uint_t baseProperties;

public:
    bool isMetaClass() { return A::P::E::get32(flags) & 1; }

    uint32_t getInstanceStart() { return A::P::E::get32(instanceStart); }
    void setInstanceStart(uint32_t newStart) { A::P::E::set32(instanceStart, newStart); }
    
    uint32_t getInstanceSize() { return A::P::E::get32(instanceSize.instanceSize); }
    void setInstanceSize(uint32_t newSiz) { A::P::E::set32(instanceSize.instanceSize, newSiz); }

    objc_method_list_t<A> *getMethodList(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(baseMethods)); }

    objc_protocol_list_t<A> *getProtocolList(SharedCache<A>* cache) const { return (objc_protocol_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(baseProtocols)); }

    objc_ivar_list_t<A> *getIvarList(SharedCache<A>* cache) const { return (objc_ivar_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(ivars)); }
    
    objc_property_list_t<A> *getPropertyList(SharedCache<A>* cache) const { return (objc_property_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(baseProperties)); }

    const char * getName(SharedCache<A>* cache) const { return (const char *)cache->mappedAddressForVMAddress(A::P::getP(name)); }

    void setMethodList(SharedCache<A>* cache, objc_method_list_t<A>* mlist) {
        A::P::setP(baseMethods, cache->VMAddressForMappedAddress(mlist));
    }

    void setProtocolList(SharedCache<A>* cache, objc_protocol_list_t<A>* protolist) {
        A::P::setP(baseProtocols, cache->VMAddressForMappedAddress(protolist));
    }
 
    void setPropertyList(SharedCache<A>* cache, objc_property_list_t<A>* proplist) {
        A::P::setP(baseProperties, cache->VMAddressForMappedAddress(proplist));
    }
	
	void addMethodListPointer(std::vector<void*>& pointersToAdd) {
		pointersToAdd.push_back(&this->baseMethods);
	}
	
	void addPropertyListPointer(std::vector<void*>& pointersToAdd) {
		pointersToAdd.push_back(&this->baseProperties);
	}
	
	void addProtocolListPointer(std::vector<void*>& pointersToAdd) {
		pointersToAdd.push_back(&this->baseProtocols);
	}
};

template <typename A>
class objc_class_t {
    typename A::P::uint_t isa;
    typename A::P::uint_t superclass;
    typename A::P::uint_t method_cache;
    typename A::P::uint_t vtable;
    typename A::P::uint_t data;

public:
    bool isMetaClass(SharedCache<A>* cache) const { return getData(cache)->isMetaClass(); }

    objc_class_t<A> *getIsa(SharedCache<A> *cache) const { return (objc_class_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(isa)); }

    objc_class_t<A> *getSuperclass(SharedCache<A> *cache) const { return (objc_class_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(superclass)); }
    
    objc_class_data_t<A> *getData(SharedCache<A>* cache) const { return (objc_class_data_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(data)); }

    objc_method_list_t<A> *getMethodList(SharedCache<A>* cache) const { return getData(cache)->getMethodList(cache); }

    objc_protocol_list_t<A> *getProtocolList(SharedCache<A>* cache) const { return getData(cache)->getProtocolList(cache); }

    objc_property_list_t<A> *getPropertyList(SharedCache<A>* cache) const { return getData(cache)->getPropertyList(cache); }

    const char * getName(SharedCache<A>* cache) const { 
        return getData(cache)->getName(cache);
    }

    void setMethodList(SharedCache<A>* cache, objc_method_list_t<A>* mlist) {
        getData(cache)->setMethodList(cache, mlist);
    }

    void setProtocolList(SharedCache<A>* cache, objc_protocol_list_t<A>* protolist) {
        getData(cache)->setProtocolList(cache, protolist);
    }

    void setPropertyList(SharedCache<A>* cache, objc_property_list_t<A>* proplist) {
        getData(cache)->setPropertyList(cache, proplist);
    }
	
	void addMethodListPointer(SharedCache<A>* cache, std::vector<void*>& pointersToAdd) {
        getData(cache)->addMethodListPointer(pointersToAdd);
	}
	
	void addPropertyListPointer(SharedCache<A>* cache, std::vector<void*>& pointersToAdd) {
        getData(cache)->addPropertyListPointer(pointersToAdd);
	}
	
	void addProtocolListPointer(SharedCache<A>* cache, std::vector<void*>& pointersToAdd) {
        getData(cache)->addProtocolListPointer(pointersToAdd);
	}
	
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

    const char * getName(SharedCache<A> *cache) const { return (const char *)cache->mappedAddressForVMAddress(A::P::getP(name)); }

    objc_class_t<A> *getClass(SharedCache<A> *cache) const { return (objc_class_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(cls)); }

    objc_method_list_t<A> *getInstanceMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(instanceMethods)); }

    objc_method_list_t<A> *getClassMethods(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(classMethods)); }

    objc_protocol_list_t<A> *getProtocols(SharedCache<A>* cache) const { return (objc_protocol_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(protocols)); }
 
    objc_property_list_t<A> *getInstanceProperties(SharedCache<A>* cache) const { return (objc_property_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(instanceProperties)); }

	void getPointers(std::set<void*>& pointersToRemove) {
		pointersToRemove.insert(&name);
		pointersToRemove.insert(&cls);
		pointersToRemove.insert(&instanceMethods);
		pointersToRemove.insert(&classMethods);
		pointersToRemove.insert(&protocols);
		pointersToRemove.insert(&instanceProperties);
	}


};

template <typename A>
class objc_message_ref_t {
    typename A::P::uint_t imp;
    typename A::P::uint_t sel;

public:
    typename A::P::uint_t getName() const { return A::P::getP(sel); }

    void setName(typename A::P::uint_t newName) { A::P::setP(sel, newName); }
};

// Call visitor.visitIvar() on every ivar in a given class.
template <typename A, typename V>
class IvarWalker {
    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;
    V& ivarVisitor;
public:
    
    IvarWalker(V& visitor) : ivarVisitor(visitor) { }
    
    void walk(SharedCache<A>* cache, const macho_header<P>* header, objc_class_t<A> *cls)
    {
        objc_class_data_t<A> *data = cls->getData(cache);
        objc_ivar_list_t<A> *ivars = data->getIvarList(cache);
        if (ivars) {
            for (pint_t i = 0; i < ivars->getCount(); i++) {
                objc_ivar_t<A>& ivar = ivars->get(i);
                //fprintf(stderr, "visiting ivar: %s\n", ivar.getName(cache));
                ivarVisitor.visitIvar(cache, header, cls, &ivar);
            }
        } else {
            //fprintf(stderr, "no ivars\n");
        }
    }
    
    void visitClass(SharedCache<A>* cache, const macho_header<P>* header, objc_class_t<A> *cls)
    {
        walk(cache, header, cls);
    }
};

// Call visitor.visitClass() on every class.
template <typename A, typename V>
class ClassWalker {
    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;
    V& classVisitor;
public:
    
    ClassWalker(V& visitor) : classVisitor(visitor) { }
    
    void walk(SharedCache<A>* cache, const macho_header<P>* header)
    {   
        PointerSection<A, objc_class_t<A> *> 
        classes(cache, header, "__DATA", "__objc_classlist");
        
        for (pint_t i = 0; i < classes.count(); i++) {
            objc_class_t<A> *cls = classes.get(i);
            //fprintf(stderr, "visiting class: %s\n", cls->getName(cache));
            classVisitor.visitClass(cache, header, cls);
        }
    }
};


// Call visitor.visitMethodList(mlist) on every method list in a header.
template <typename A, typename V>
class MethodListWalker {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    V& mVisitor;

public: 
    
    MethodListWalker(V& visitor) : mVisitor(visitor) { }

    void walk(SharedCache<A>* cache, const macho_header<P>* header, bool walkProtocols)
    {   
        // Method lists in classes
        PointerSection<A, objc_class_t<A> *> 
            classes(cache, header, "__DATA", "__objc_classlist");
            
        for (pint_t i = 0; i < classes.count(); i++) {
            objc_class_t<A> *cls = classes.get(i);
            objc_method_list_t<A> *mlist;
            if ((mlist = cls->getMethodList(cache))) {
                mVisitor.visitMethodList(mlist);
            }
            if ((mlist = cls->getIsa(cache)->getMethodList(cache))) {
                mVisitor.visitMethodList(mlist);
            }
        }
        
        // Method lists from categories
        PointerSection<A, objc_category_t<A> *> 
            cats(cache, header, "__DATA", "__objc_catlist");
        for (pint_t i = 0; i < cats.count(); i++) {
            objc_category_t<A> *cat = cats.get(i);
            objc_method_list_t<A> *mlist;
            if ((mlist = cat->getInstanceMethods(cache))) {
                mVisitor.visitMethodList(mlist);
            }
            if ((mlist = cat->getClassMethods(cache))) {
                mVisitor.visitMethodList(mlist);
            }
        }

		// Method description lists from protocols
		if ( walkProtocols ) {
			PointerSection<A, objc_protocol_t<A> *> 
				protocols(cache, header, "__DATA", "__objc_protolist");
			for (pint_t i = 0; i < protocols.count(); i++) {
				objc_protocol_t<A> *proto = protocols.get(i);
				objc_method_list_t<A> *mlist;
				if ((mlist = proto->getInstanceMethods(cache))) {
					mVisitor.visitMethodList(mlist);
				}
				if ((mlist = proto->getClassMethods(cache))) {
					mVisitor.visitMethodList(mlist);
				}
				if ((mlist = proto->getOptionalInstanceMethods(cache))) {
					mVisitor.visitMethodList(mlist);
				}
				if ((mlist = proto->getOptionalClassMethods(cache))) {
					mVisitor.visitMethodList(mlist);
				}
			}
		}
    }
};


// Update selector references. The visitor performs recording and uniquing.
template <typename A, typename V>
class SelectorOptimizer {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    V& mVisitor;

    friend class MethodListWalker< A, SelectorOptimizer<A,V> >;
    void visitMethodList(objc_method_list_t<A> *mlist)
    {
        // Gather selectors. Update method names.
        for (pint_t m = 0; m < mlist->getCount(); m++) {
            pint_t oldValue = mlist->get(m).getName();
            pint_t newValue = mVisitor.visit(oldValue);
            mlist->get(m).setName(newValue);
        }
        // Do not setFixedUp: the methods are not yet sorted.
    }

public:

    SelectorOptimizer(V& visitor) : mVisitor(visitor) { }

    void optimize(SharedCache<A>* cache, const macho_header<P>* header)
    {
        // method lists of all kinds
        MethodListWalker< A, SelectorOptimizer<A,V> > mw(*this);
        mw.walk(cache, header, true);
        
        // @selector references
        PointerSection<A, const char *> 
            selrefs(cache, header, "__DATA", "__objc_selrefs");
        for (pint_t i = 0; i < selrefs.count(); i++) {
            pint_t oldValue = selrefs.getUnmapped(i);
            pint_t newValue = mVisitor.visit(oldValue);
            selrefs.set(i, newValue);
        }

        // message references
        ArraySection<A, objc_message_ref_t<A> > 
            msgrefs(cache, header, "__DATA", "__objc_msgrefs");
        for (pint_t i = 0; i < msgrefs.count(); i++) {
            objc_message_ref_t<A>& msg = msgrefs.get(i);
            pint_t oldValue = msg.getName();
            pint_t newValue = mVisitor.visit(oldValue);
            msg.setName(newValue);
        }
    }
};


// Update selector references. The visitor performs recording and uniquing.
template <typename A>
class IvarOffsetOptimizer {
    typedef typename A::P P;

    uint32_t slide;
    uint32_t maxAlignment;

    uint32_t fOptimized;

public:
    
    IvarOffsetOptimizer() : fOptimized(0) { }

    size_t optimized() const { return fOptimized; }
    
    // dual purpose ivar visitor function
    // if slide!=0 then slides the ivar by that amount, otherwise computes maxAlignment
    void visitIvar(SharedCache<A>* cache, const macho_header<P>* /*unused, may be NULL*/, objc_class_t<A> *cls, objc_ivar_t<A> *ivar)
    {
        if (slide == 0) {
            uint32_t alignment = ivar->getAlignment();
            if (alignment > maxAlignment) maxAlignment = alignment;
        } else {
            // skip anonymous bitfields
            if (ivar->hasOffset()) {
                uint32_t oldOffset = (uint32_t)ivar->getOffset(cache);
                ivar->setOffset(cache, oldOffset + slide);
                fOptimized++;
                //fprintf(stderr, "%d -> %d for %s.%s\n", oldOffset, oldOffset + slide, cls->getName(cache), ivar->getName(cache));
            } else {
                //fprintf(stderr, "NULL offset\n");
            }
        }
    }
    
    // Class visitor function. Evaluates whether to slide ivars and performs slide if needed.
    // The slide algorithm is also implemented in objc. Any changes here should be reflected there also.
    void visitClass(SharedCache<A>* cache, const macho_header<P>* /*unused, may be NULL*/, objc_class_t<A> *cls)
    {
        objc_class_t<A> *super = cls->getSuperclass(cache);
        if (super) {
            // Recursively visit superclasses to ensure we have the correct superclass start
            // Note that we don't need the macho_header, so just pass NULL.
            visitClass(cache, NULL, super);

            objc_class_data_t<A> *data = cls->getData(cache);
            objc_class_data_t<A> *super_data = super->getData(cache);
            int32_t diff = super_data->getInstanceSize() - data->getInstanceStart();
            if (diff > 0) {
                IvarWalker<A, IvarOffsetOptimizer<A> > ivarVisitor(*this);
                maxAlignment = 0;
                slide = 0;
                
                // This walk computes maxAlignment
                ivarVisitor.walk(cache, NULL, cls);

                // Compute a slide value that preserves that alignment
                uint32_t alignMask = maxAlignment - 1;
                if (diff & alignMask) diff = (diff + alignMask) & ~alignMask;

                // Slide all of this class's ivars en masse
                slide = diff;
                if (slide != 0) {
                    //fprintf(stderr, "Sliding ivars in %s by %u (superclass was %d, now %d)\n", cls->getName(cache), slide, data->getInstanceStart(), super_data->getInstanceSize());
                    ivarVisitor.walk(cache, NULL, cls);
                    data->setInstanceStart(data->getInstanceStart() + slide);
                    data->setInstanceSize(data->getInstanceSize() + slide);
                }
            }
        }
    }
    
    // Enumerates objc classes in the module and performs any ivar slides
    void optimize(SharedCache<A>* cache, const macho_header<P>* header)
    {
        // The slide code cannot fix up GC layout strings so skip modules that support ore require GC
        const macho_section<P> *imageInfoSection = header->getSection("__DATA", "__objc_imageinfo");
        if (imageInfoSection) {
            objc_image_info<A> *info = (objc_image_info<A> *)cache->mappedAddressForVMAddress(imageInfoSection->addr());
            if (!info->supportsGCFlagSet() && !info->requiresGCFlagSet()) {
                ClassWalker<A, IvarOffsetOptimizer<A> > classVisitor(*this);
                classVisitor.walk(cache, header);
            } else {
                //fprintf(stderr, "GC support present - skipped module\n");
            }
        }
    }
};


// Sort methods in place by selector.
template <typename A>
class MethodListSorter {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    uint32_t fOptimized;

    friend class MethodListWalker<A, MethodListSorter<A> >;
    void visitMethodList(objc_method_list_t<A> *mlist)
    {
        typename objc_method_t<A>::SortBySELAddress sorter;
        std::stable_sort(mlist->begin(), mlist->end(), sorter);
        mlist->setFixedUp();
        fOptimized++;
    }

public:
    MethodListSorter() : fOptimized(0) { }

    size_t optimized() const { return fOptimized; }

    void optimize(SharedCache<A>* cache, macho_header<P>* header)
    {
        MethodListWalker<A, MethodListSorter<A> > mw(*this);
        mw.walk(cache, header, false /* don't sort protocol method lists*/);
    }
};


template <typename A>
class HeaderInfoOptimizer {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    objc_header_info_t<A>* fHinfos;
    size_t fCount;

public:
    HeaderInfoOptimizer() : fHinfos(0), fCount(0) { }

    const char *init(size_t count, uint8_t*& buf, size_t& bufSize)
    {
        if (count == 0) return NULL;

        if (bufSize < 2*sizeof(uint32_t) + count*sizeof(objc_header_info_t<A>)) {
            return "libobjc's read/write section is too small (metadata not optimized)";
        }

        uint32_t *buf32 = (uint32_t *)buf;
        A::P::E::set32(buf32[0], count);
        A::P::E::set32(buf32[1], sizeof(objc_header_info_t<A>));
        fHinfos = (objc_header_info_t<A>*)(buf32+2);

        size_t total = sizeof(uint32_t) + count*sizeof(objc_header_info_t<A>);
        buf += total;
        bufSize -= total;

        return NULL;
    }

    void update(SharedCache<A>* cache, const macho_header<P>* mh, std::vector<void*>& pointersInData)
    { 
        objc_header_info_t<A>* hi = new(&fHinfos[fCount++]) objc_header_info_t<A>(cache, mh);
        hi->addPointers(pointersInData);
    }

    objc_header_info_t<A>* hinfoForHeader(SharedCache<A>* cache, const macho_header<P>* mh)
    {
        // fixme could be binary search
        pint_t mh_vmaddr = cache->VMAddressForMappedAddress(mh);
        for (size_t i = 0; i < fCount; i++) {
            objc_header_info_t<A>* hi = &fHinfos[i];
            if (hi->header_vmaddr() == mh_vmaddr) return hi;
        }
        return NULL;
    }
};
