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
    const T& operator * () const { return *current; }
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

    // use newIvarList instead
    void* operator new (size_t) { return NULL; }
    void* operator new (size_t, void* buf) { return buf; }

public:

    typedef entsize_iterator< A, objc_ivar_t<A>, objc_ivar_list_t<A> > ivar_iterator;

    uint32_t getCount() const { return A::P::E::get32(count); }

	uint32_t getEntsize() const { return A::P::E::get32(entsize); }

    objc_ivar_t<A>& get(typename A::P::pint_t i) const { return *(objc_ivar_t<A> *)((uint8_t *)&first + i * A::P::E::get32(entsize)); }

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
    objc_method_list_t<A> *getMethodList(SharedCache<A>* cache) const { return (objc_method_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(baseMethods)); }

    objc_protocol_list_t<A> *getProtocolList(SharedCache<A>* cache) const { return (objc_protocol_list_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(baseProtocols)); }

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
    objc_class_t<A> *getIsa(SharedCache<A> *cache) const { return (objc_class_t<A> *)cache->mappedAddressForVMAddress(A::P::getP(isa)); }

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


// Call visitor.visitMethodList(mlist) on every method list in a header.
template <typename A, typename V>
class MethodListWalker {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    V& mVisitor;

public: 
    
    MethodListWalker(V& visitor) : mVisitor(visitor) { }

    void walk(SharedCache<A>* cache, const macho_header<P>* header)
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
        mw.walk(cache, header);
        
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

        // Mark image_info
        const macho_section<P> *imageInfoSection = 
            header->getSection("__DATA", "__objc_imageinfo");
        if (imageInfoSection) {
            objc_image_info<A> *info = (objc_image_info<A> *)
                cache->mappedAddressForVMAddress(imageInfoSection->addr());
            info->setSelectorsPrebound();
        }
    }
};


// Sort methods in place by selector.
template <typename A>
class MethodListSorter {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    friend class MethodListWalker<A, MethodListSorter<A> >;
    void visitMethodList(objc_method_list_t<A> *mlist)
    {
        typename objc_method_t<A>::SortBySELAddress sorter;
        std::stable_sort(mlist->begin(), mlist->end(), sorter);
        mlist->setFixedUp();
    }

public:

    void optimize(SharedCache<A>* cache, macho_header<P>* header)
    {
        MethodListWalker<A, MethodListSorter<A> > mw(*this);
        mw.walk(cache, header);
    }
};


// Attach categories to classes in the same framework. 
// Merge method and protocol and property lists.
template <typename A>
class CategoryAttacher {

    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    uint8_t *mBytes;
    ssize_t mBytesFree;
    ssize_t mBytesUsed;

    size_t mCategoriesAttached;

    bool segmentContainsPointer(SharedCache<A>* cache, 
                                const macho_segment_command<P>* seg, void *ptr)
    {
        if (!seg) return false;
        void *start = (void*)
            cache->mappedAddressForVMAddress(seg->vmaddr());
        void *end   = (uint8_t *)start + seg->filesize();
        return (ptr >= start  &&  ptr < end);
    }

    bool headerContainsPointer(SharedCache<A>* cache, 
                               macho_header<P>* header, void *ptr)
    {
        return 
            segmentContainsPointer(cache, header->getSegment("__DATA"), ptr) ||
            segmentContainsPointer(cache, header->getSegment("__TEXT"), ptr) ||
            segmentContainsPointer(cache, header->getSegment("__OBJC"), ptr);
    }

    struct pointer_hash {
        size_t operator () (void* ptr) const { 
            return __gnu_cxx::hash<long>()((long)ptr); 
        }
    };

    typedef std::deque<objc_category_t<A>*> CategoryList;
    typedef std::vector<uint64_t> CategoryRefs;

    struct ClassChanges {
        CategoryList categories;
        CategoryRefs catrefs;
        
        objc_method_list_t<A>* instanceMethods;
        objc_method_list_t<A>* classMethods;
        objc_protocol_list_t<A>* protocols;
        objc_property_list_t<A>* instanceProperties;

        ClassChanges() 
            : instanceMethods(NULL), classMethods(NULL), 
              protocols(NULL), instanceProperties(NULL)
        { }

        ~ClassChanges() { 
            if (instanceMethods) delete instanceMethods;
            if (classMethods) delete classMethods;
            if (protocols) delete protocols;
            if (instanceProperties) delete instanceProperties;
        }
    };

    typedef __gnu_cxx::hash_map<objc_class_t<A>*, ClassChanges, pointer_hash> ClassMap;

    class RangeArray {
        typedef std::pair<uint8_t*,uint32_t> Range;
        std::deque<Range> ranges;

        class SizeFits {
        private:
            uint32_t mSize;
        public:
            SizeFits(uint32_t size) : mSize(size) { } 
            bool operator() (const Range& r) { 
                return r.second >= mSize;
            }
        };

        struct AddressComp {
            bool operator() (const Range& lhs, const Range& rhs) {
                return (lhs.first < rhs.first);
            }
        };
    public:
        RangeArray() { }
        void add(void* p, uint32_t size) {
            add(Range((uint8_t *)p, size));
        }
        void add(const Range& r) {
            // find insertion point
            std::deque<Range>::iterator iter;
            iter = upper_bound(ranges.begin(), ranges.end(), r, AddressComp());
            // coalesce
            // fixme doesn't fully coalesce if new range exactly fills a gap
            if (iter != ranges.begin()) {
                std::deque<Range>::iterator prev = iter - 1;
                if ((*prev).first + (*prev).second == r.first) {
                    (*prev).second += r.second;
                    return;
                }
            }
            if (iter != ranges.end()  &&  iter+1 != ranges.end()) {
                std::deque<Range>::iterator next = iter + 1;
                if (r.first + r.second == (*next).first) {
                    (*next).second += r.second;
                    (*next).first = r.first;
                    return;
                }
            }
            ranges.insert(iter, r);
        }

        uint8_t* remove(uint32_t size) {
            // first-fit search
            // this saves 50-75% of space overhead; 
            // a better algorithm might do better

            std::deque<Range>::iterator iter;
            iter = find_if(ranges.begin(), ranges.end(), SizeFits(size));
            if (iter == ranges.end()) {
                return NULL;
            }

            Range& found = *iter;
            uint8_t *result = found.first;
            if (found.second > size) {
                // keep leftovers
                found.first += size;
                found.second -= size;
            } else {
                ranges.erase(iter);
            }

            return result;
        }
    };

    void copyMethods(typename objc_method_list_t<A>::method_iterator& dst, 
                     const objc_method_list_t<A>* srcList)
    {
        objc_method_list_t<A>::method_iterator::
            overwrite(dst, srcList);
    }

    void copyProperties(typename objc_property_list_t<A>::property_iterator& dst, 
                        const objc_property_list_t<A>* srcList)
    {
        objc_property_list_t<A>::property_iterator::
            overwrite(dst, srcList);
    }

    void copyProtocols(objc_protocol_list_t<A>* dst, pint_t& dstIndex,
                       const objc_protocol_list_t<A>* src)
    {
        dst->overwrite(dstIndex, src);
    }

	class InSet
	{
	public:
		InSet(std::set<void*>& deadPointers) : _deadPointers(deadPointers) {}

		bool operator()(void* ptr) const {
			return ( _deadPointers.count(ptr) != 0 );
		}

	private:
		std::set<void*>& _deadPointers;
	};

public:

    CategoryAttacher(uint8_t *bytes, ssize_t bytesFree) 
        : mBytes(bytes), mBytesFree(bytesFree)
        , mBytesUsed(0), mCategoriesAttached(0) 
    { }

    size_t count() const { return mCategoriesAttached; }

    const char *optimize(SharedCache<A>* cache, macho_header<P>* header, std::vector<void*>& pointersInData)
    {
        // Build class=>cateories mapping.
        // Disregard target classes that aren't in this binary.

        ClassMap map;

		PointerSection<A, objc_category_t<A> *> 
            nlcatsect(cache, header, "__DATA", "__objc_nlcatlist");
        PointerSection<A, objc_category_t<A> *> 
            catsect(cache, header, "__DATA", "__objc_catlist");
        for (pint_t i = 0; i < catsect.count(); i++) {
            objc_category_t<A> *cat = catsect.get(i);
            objc_class_t<A> *cls = cat->getClass(cache);
            if (!cls) continue;
            if (!headerContainsPointer(cache, header, cls)) continue;
			if ( nlcatsect.count() !=0 ) {
				// don't optimize categories also in __objc_nlcatlist
				bool alsoInNlcatlist = false;
				for (pint_t nli = 0; nli < nlcatsect.count(); nli++) {
					if ( nlcatsect.get(nli) == cat ) {
						//fprintf(stderr, "skipping cat in __objc_nlcatlist for mh=%p\n", header);
						alsoInNlcatlist = true;
						break;
					}
				}
				if ( alsoInNlcatlist ) 
					continue;
			}

            // The LAST category found is the FIRST to be processed later.
            map[cls].categories.push_front(cat);

            // We don't care about the category reference order.
            map[cls].catrefs.push_back(i);
        }

        if (map.size() == 0) {
            // No attachable categories.
            return NULL;
        }

        // Process each class.
        // Each class is all-or-nothing: either all of its categories 
        // are attached successfully, or none of them are. This preserves 
        // cache validity if we run out of space for more reallocations.

        // unusedMemory stores memory ranges evacuated by now-unused metadata.
        // It is available for re-use by other newly-added metadata.
        // fixme could packing algorithm be improved?
        RangeArray unusedMemory;

        ssize_t reserve = 0;

        // First: build new aggregated lists on the heap.
        // Require enough space in mBytes for all of it.

		std::set<void*> pointersToRemove;
        for (typename ClassMap::iterator i = map.begin(); 
             i != map.end(); 
             ++i) 
        {
            objc_class_t<A>* cls = i->first;
            objc_class_t<A>* meta = cls->getIsa(cache);
            ClassChanges& changes = i->second;
            CategoryList& cats = changes.categories;

            // Count memory needed for all categories on this class.

            uint32_t methodEntsize = 0;
            uint32_t propertyEntsize = 0;
            objc_method_list_t<A>* mlist;
            objc_property_list_t<A>* proplist;
            objc_protocol_list_t<A>* protolist;
            uint32_t instanceMethodsCount = 0;
            uint32_t classMethodsCount = 0;
            uint32_t instancePropertyCount = 0;
            pint_t protocolCount = 0;
            bool addedInstanceMethods = false;
            bool addedClassMethods = false;
            bool addedInstanceProperties = false;
            bool addedProtocols = false;

            mlist = cls->getMethodList(cache);
            if (mlist) {
                instanceMethodsCount = mlist->getCount();
                methodEntsize = 
                    std::max(methodEntsize, mlist->getEntsize());
            }

            mlist = meta->getMethodList(cache);
            if (mlist) {
                classMethodsCount = mlist->getCount();
                methodEntsize = 
                    std::max(methodEntsize, mlist->getEntsize());
            }

            proplist = cls->getPropertyList(cache);
            if (proplist) {
                instancePropertyCount = proplist->getCount();
                propertyEntsize = 
                    std::max(propertyEntsize, proplist->getEntsize());
            }

            protolist = cls->getProtocolList(cache);
            if (protolist) {
                protocolCount = protolist->getCount();
            }

            typename CategoryList::iterator j;
            for (j = cats.begin(); j != cats.end(); ++j) {
                objc_category_t<A>* cat = *j;

                mlist = cat->getInstanceMethods(cache);
                if (mlist  &&  mlist->getCount() > 0) {
                    addedInstanceMethods = true;
                    instanceMethodsCount += mlist->getCount();
                    methodEntsize = 
                        std::max(methodEntsize, mlist->getEntsize());
                }

                mlist = cat->getClassMethods(cache);
                if (mlist  &&  mlist->getCount() > 0) {
                    addedClassMethods = true;
                    classMethodsCount += mlist->getCount();
                    methodEntsize = 
                        std::max(methodEntsize, mlist->getEntsize());
                }

                proplist = cat->getInstanceProperties(cache);
                if (proplist  &&  proplist->getCount() > 0) {
                    addedInstanceProperties = true;
                    instancePropertyCount += proplist->getCount();
                    propertyEntsize = 
                        std::max(propertyEntsize, proplist->getEntsize());
                }

                protolist = cat->getProtocols(cache);
                if (protolist  &&  protolist->getCount() > 0) {
                    addedProtocols = true;
                    protocolCount += protolist->getCount();
                }
            }

            // Allocate memory for aggregated lists. 
            // Reserve the same amount of space from mBytes.

            if (addedInstanceMethods) {
                changes.instanceMethods = objc_method_list_t<A>::newMethodList(instanceMethodsCount, methodEntsize);
                reserve = P::round_up(reserve + changes.instanceMethods->byteSize());
            }
            if (addedClassMethods) {
                changes.classMethods = objc_method_list_t<A>::newMethodList(classMethodsCount, methodEntsize);
                reserve = P::round_up(reserve + changes.classMethods->byteSize());
            }
            if (addedInstanceProperties) {
                changes.instanceProperties = objc_property_list_t<A>::newPropertyList(instancePropertyCount, propertyEntsize);
                reserve = P::round_up(reserve + changes.instanceProperties->byteSize());
            }
            if (addedProtocols) {
                changes.protocols = objc_protocol_list_t<A>::newProtocolList(protocolCount);
                reserve = P::round_up(reserve + changes.protocols->byteSize());
            }

			// Merge. The LAST category's contents ends up FIRST in each list.
			// The aggregated lists are not sorted; a future pass does that.

            typename objc_method_list_t<A>::method_iterator newInstanceMethods;
            typename objc_method_list_t<A>::method_iterator newClassMethods;
            typename objc_property_list_t<A>::property_iterator newInstanceProperties;
            pint_t newProtocolIndex;

            if (addedInstanceMethods) {
                newInstanceMethods = changes.instanceMethods->begin();
            }
            if (addedClassMethods) {
                newClassMethods = changes.classMethods->begin();
            }
            if (addedInstanceProperties) {
                newInstanceProperties = changes.instanceProperties->begin();
            }
            if (addedProtocols) {
                newProtocolIndex = 0;
            }
            
            for (j = cats.begin(); j != cats.end(); ++j) {
                objc_category_t<A>* cat = *j;

                mlist = cat->getInstanceMethods(cache);
                if (mlist) {
                    copyMethods(newInstanceMethods, mlist);
					mlist->getPointers(pointersToRemove);
                    unusedMemory.add(mlist, mlist->byteSize());
                }

                mlist = cat->getClassMethods(cache);
                if (mlist) {
                    copyMethods(newClassMethods, mlist);
  					mlist->getPointers(pointersToRemove);
					unusedMemory.add(mlist, mlist->byteSize());
                }

                proplist = cat->getInstanceProperties(cache);
                if (proplist) {
                    copyProperties(newInstanceProperties, proplist);
  					proplist->getPointers(pointersToRemove);
					unusedMemory.add(proplist, proplist->byteSize());
                }

                protolist = cat->getProtocols(cache);
                if (protolist) {
                    copyProtocols(changes.protocols, newProtocolIndex, protolist);
   					protolist->getPointers(pointersToRemove);
					unusedMemory.add(protolist, protolist->byteSize());
                }

				cat->getPointers(pointersToRemove);
				unusedMemory.add(cat, sizeof(*cat));                
            }

            if (addedInstanceMethods && (mlist = cls->getMethodList(cache))) {
                copyMethods(newInstanceMethods, mlist);
				mlist->getPointers(pointersToRemove);
				unusedMemory.add(mlist, mlist->byteSize());
            }
            if (addedClassMethods && (mlist = meta->getMethodList(cache))) {
                copyMethods(newClassMethods, mlist);
				mlist->getPointers(pointersToRemove);
                unusedMemory.add(mlist, mlist->byteSize());
            }
            if (addedInstanceProperties && (proplist = cls->getPropertyList(cache))) {
                copyProperties(newInstanceProperties, proplist);
 				proplist->getPointers(pointersToRemove);
				unusedMemory.add(proplist, proplist->byteSize());
            }
            if (addedProtocols && (protolist = cls->getProtocolList(cache))) {
                copyProtocols(changes.protocols, newProtocolIndex, protolist);
 				protolist->getPointers(pointersToRemove);
                unusedMemory.add(protolist, protolist->byteSize());
            }
        }

		if (reserve > mBytesFree) {
			return "insufficient space for category data (metadata not optimized)";
		}

		// update cache slide info and remove areas now longer containing pointers
		//fprintf(stderr, "found %lu pointers in objc structures being moved\n", pointersToRemove.size());
		pointersInData.erase(std::remove_if(pointersInData.begin(), pointersInData.end(), InSet(pointersToRemove)), pointersInData.end());
		

		// All lists are now built.
        // mBytes is big enough to hold everything if necessary.
        // Everything in unusedMemory is now available for re-use.
        // The original metadata is still untouched.

        // Second: write lists into mBytes and unusedMemory, 
        // then disconnect categories.

        for (typename ClassMap::iterator i = map.begin(); 
             i != map.end(); 
             ++i) 
        {
            objc_class_t<A>* cls = i->first;
            objc_class_t<A>* meta = cls->getIsa(cache);
            ClassChanges& changes = i->second;

            // Write lists.
            
            if (changes.instanceMethods) {
                uint8_t *bytes;
                uint32_t size = changes.instanceMethods->byteSize();
                if (! (bytes = unusedMemory.remove(size))) {
                    bytes = mBytes + mBytesUsed;
                    mBytesFree -= size;
                    mBytesUsed += size;
                }
                memcpy(bytes, changes.instanceMethods, size);
				objc_method_list_t<A>::addPointers(bytes, pointersInData);
				cls->setMethodList(cache, (objc_method_list_t<A> *)bytes);
				cls->addMethodListPointer(cache, pointersInData);
            }
            
            if (changes.classMethods) {
                uint8_t *bytes;
                uint32_t size = changes.classMethods->byteSize();
                if (! (bytes = unusedMemory.remove(size))) {
                    bytes = mBytes + mBytesUsed;
                    mBytesFree -= size;
                    mBytesUsed += size;
                }
                memcpy(bytes, changes.classMethods, size);
 				objc_method_list_t<A>::addPointers(bytes, pointersInData);
				meta->setMethodList(cache, (objc_method_list_t<A> *)bytes);
				meta->addMethodListPointer(cache, pointersInData);
            }
            
            if (changes.instanceProperties) {
                uint8_t *bytes;
                uint32_t size = changes.instanceProperties->byteSize();
                if (! (bytes = unusedMemory.remove(size))) {
                    bytes = mBytes + mBytesUsed;
                    mBytesFree -= size;
                    mBytesUsed += size;
                }
                memcpy(bytes, changes.instanceProperties, size);
 				objc_property_list_t<A>::addPointers(bytes, pointersInData);
                cls->setPropertyList(cache, (objc_property_list_t<A> *)bytes);
  				cls->addPropertyListPointer(cache, pointersInData);
          }

            if (changes.protocols) {
                uint8_t *bytes;
                uint32_t size = changes.protocols->byteSize();
                if (! (bytes = unusedMemory.remove(size))) {
                    bytes = mBytes + mBytesUsed;
                    mBytesFree -= size;
                    mBytesUsed += size;
                }
                memcpy(bytes, changes.protocols, size);
                cls->setProtocolList(cache, (objc_protocol_list_t<A> *)bytes);
  				objc_protocol_list_t<A>::addPointers(bytes, pointersInData);
   				cls->addProtocolListPointer(cache, pointersInData);
				meta->setProtocolList(cache, (objc_protocol_list_t<A> *)bytes);
				meta->addProtocolListPointer(cache, pointersInData);
          }

            // Disavow all knowledge of the categories.

            for (typename CategoryRefs::iterator j = changes.catrefs.begin();
                 j != changes.catrefs.end();
                 ++j)
            {
                catsect.set(*j, 0);
            }

            mCategoriesAttached += changes.categories.size();
        }

        catsect.removeNulls();

        return NULL;
    }

    ssize_t bytesUsed() { return mBytesUsed; }
};
