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
#ifndef __MACH_O_TRIE__
#define __MACH_O_TRIE__


#include "MachOFileAbstraction.hpp"

namespace mach_o {
namespace trie {

struct Edge
{
					Edge(const char* s, struct Node* n) : fSubString(s), fChild(n) { }
					~Edge() {  }
	const char*		fSubString;
	struct Node*	fChild;
	
};

struct Node
{
						Node(const char* s) : fCummulativeString(s), fAddress(0), fFlags(0), fOrdered(false), 
											fHaveExportInfo(false), fTrieOffset(0) {}
						~Node() { }
	const char*			fCummulativeString;
	std::vector<Edge>	fChildren;
	uint64_t			fAddress;
	uint32_t			fFlags;
	bool				fOrdered;
	bool				fHaveExportInfo;
	uint32_t			fTrieOffset;
	
	void addSymbol(const char* fullStr, uint64_t address, uint32_t flags) {
		const char* partialStr = &fullStr[strlen(fCummulativeString)];
		for (std::vector<Edge>::iterator it = fChildren.begin(); it != fChildren.end(); ++it) {
			Edge& e = *it;
			int subStringLen = strlen(e.fSubString);
			if ( strncmp(e.fSubString, partialStr, subStringLen) == 0 ) {
				// already have matching edge, go down that path
				e.fChild->addSymbol(fullStr, address, flags);
				return;
			}
			else {
				for (int i=subStringLen-1; i > 0; --i) {
					if ( strncmp(e.fSubString, partialStr, i) == 0 ) {
						// found a common substring, splice in new node
						//  was A -> C,  now A -> B -> C
						char* bNodeCummStr = strdup(e.fChild->fCummulativeString);
						bNodeCummStr[strlen(bNodeCummStr)+i-subStringLen] = '\0';
						//node* aNode = this;
						Node* bNode = new Node(bNodeCummStr);
						Node* cNode = e.fChild;
						char* abEdgeStr = strdup(e.fSubString);
						abEdgeStr[i] = '\0';
						char* bcEdgeStr = strdup(&e.fSubString[i]);
						Edge& abEdge = e;
						abEdge.fSubString = abEdgeStr;
						abEdge.fChild = bNode;
						Edge bcEdge(bcEdgeStr, cNode);
						bNode->fChildren.push_back(bcEdge);
						bNode->addSymbol(fullStr, address, flags);
						return;
					}
				}
			}
		}
		// no commonality with any existing child, make a new edge that is this whole string
		Node* newNode = new Node(strdup(fullStr));
		Edge newEdge(strdup(partialStr), newNode);
		fChildren.push_back(newEdge);
		newNode->fAddress = address;
		newNode->fFlags = flags;
		newNode->fHaveExportInfo = true;
	}
	
	void addOrderedNodes(const char* name, std::vector<Node*>& orderedNodes) {
		if ( !fOrdered ) {
			orderedNodes.push_back(this);
			//fprintf(stderr, "ordered %p %s\n", this, fCummulativeString);
			fOrdered = true;
		}
		const char* partialStr = &name[strlen(fCummulativeString)];
		for (std::vector<Edge>::iterator it = fChildren.begin(); it != fChildren.end(); ++it) {
			Edge& e = *it;
			int subStringLen = strlen(e.fSubString);
			if ( strncmp(e.fSubString, partialStr, subStringLen) == 0 ) {
				// already have matching edge, go down that path
				e.fChild->addOrderedNodes(name, orderedNodes);
				return;
			}
		}
	}

	// byte for terminal node size in bytes, or 0x00 if not terminal node
	// teminal node (uleb128 flags, uleb128 addr)
	// byte for child node count
	//  each child: zero terminated substring, uleb128 node offset
	bool updateOffset(uint32_t& offset) {
		uint32_t nodeSize = 1; // byte for length of export info
		if ( fHaveExportInfo ) 
			nodeSize += uleb128_size(fFlags) + uleb128_size(fAddress);

		// add children
		++nodeSize; // byte for count of chidren
		for (std::vector<Edge>::iterator it = fChildren.begin(); it != fChildren.end(); ++it) {
			Edge& e = *it;
			nodeSize += strlen(e.fSubString) + 1 + uleb128_size(e.fChild->fTrieOffset);
		}
		bool result = (fTrieOffset != offset);
		fTrieOffset = offset;
		//fprintf(stderr, "updateOffset %p %05d %s\n", this, fTrieOffset, fCummulativeString);
		offset += nodeSize;
		// return true if fTrieOffset was changed
		return result;
	}

	void appendToStream(std::vector<uint8_t>& out) {
		if ( fHaveExportInfo ) {
			// nodes with export info: size, flags, address
			out.push_back(uleb128_size(fFlags) + uleb128_size(fAddress));
			append_uleb128(fFlags, out);
			append_uleb128(fAddress, out);
		}
		else {
			// no export info
			out.push_back(0);
		}
		// write number of children
		out.push_back(fChildren.size());
		// write each child
		for (std::vector<Edge>::iterator it = fChildren.begin(); it != fChildren.end(); ++it) {
			Edge& e = *it;
			append_string(e.fSubString, out);
			append_uleb128(e.fChild->fTrieOffset, out);
		}
	}
	
private:
	static void append_uleb128(uint64_t value, std::vector<uint8_t>& out) {
		uint8_t byte;
		do {
			byte = value & 0x7F;
			value &= ~0x7F;
			if ( value != 0 )
				byte |= 0x80;
			out.push_back(byte);
			value = value >> 7;
		} while( byte >= 0x80 );
	}
	
	static void append_string(const char* str, std::vector<uint8_t>& out) {
		for (const char* s = str; *s != '\0'; ++s)
			out.push_back(*s);
		out.push_back('\0');
	}
	
	static unsigned int	uleb128_size(uint64_t value) {
		uint32_t result = 0;
		do {
			value = value >> 7;
			++result;
		} while ( value != 0 );
		return result;
	}
	

};

inline uint64_t read_uleb128(const uint8_t*& p, const uint8_t* end) {
	uint64_t result = 0;
	int		 bit = 0;
	do {
		if (p == end)
			throw "malformed uleb128 extends beyond trie";

		uint64_t slice = *p & 0x7f;

		if (bit >= 64 || slice << bit >> bit != slice)
			throw "uleb128 too big for 64-bits";
		else {
			result |= (slice << bit);
			bit += 7;
		}
	} 
	while (*p++ & 0x80);
	return result;
}
	


struct Entry
{
	const char*		name;
	uint64_t		address;
	uint64_t		flags;
};


inline void makeTrie(const std::vector<Entry>& input, std::vector<uint8_t>& output)
{
	Node start(strdup(""));
	
	// make nodes for all exported symbols
	for (std::vector<Entry>::const_iterator it = input.begin(); it != input.end(); ++it) {
		start.addSymbol(it->name, it->address, it->flags);
	}

	// create vector of nodes
	std::vector<Node*> orderedNodes;
	orderedNodes.reserve(input.size()*2);
	for (std::vector<Entry>::const_iterator it = input.begin(); it != input.end(); ++it) {
		start.addOrderedNodes(it->name, orderedNodes);
	}
	
	// assign each node in the vector an offset in the trie stream, iterating until all uleb128 sizes have stabilized
	bool more;
	do {
		uint32_t offset = 0;
		more = false;
		for (std::vector<Node*>::iterator it = orderedNodes.begin(); it != orderedNodes.end(); ++it) {
			if ( (*it)->updateOffset(offset) )
				more = true;
		}
	} while ( more );
	
	// create trie stream
	for (std::vector<Node*>::iterator it = orderedNodes.begin(); it != orderedNodes.end(); ++it) {
		(*it)->appendToStream(output);
	}
}

struct EntryWithOffset
{
	uintptr_t		nodeOffset;
	Entry			entry;
	
	bool operator<(const EntryWithOffset& other) const { return ( nodeOffset < other.nodeOffset ); }
};



static inline void processExportNode(const uint8_t* const start, const uint8_t* p, const uint8_t* const end, 
											char* cummulativeString, int curStrOffset, std::vector<EntryWithOffset>& output) 
{
	if ( p >= end )
		throwf("malformed trie, node %p outside trie (%p -> %p)", p, start, end);
	const uint8_t terminalSize = *p++;
	const uint8_t* children = p + terminalSize;
	if ( terminalSize != 0 ) {
		EntryWithOffset e;
		e.nodeOffset = p-start;
		e.entry.name = strdup(cummulativeString);
		e.entry.flags = read_uleb128(p, end);
		e.entry.address = read_uleb128(p, end); 
		output.push_back(e);
	}
	const uint8_t childrenCount = *children++;
	const uint8_t* s = children;
	for (uint8_t i=0; i < childrenCount; ++i) {
		int edgeStrLen = 0;
		while (*s != '\0') {
			cummulativeString[curStrOffset+edgeStrLen] = *s++;
			++edgeStrLen;
		}
		cummulativeString[curStrOffset+edgeStrLen] = *s++;
		uint32_t childNodeOffet = read_uleb128(s, end);
		processExportNode(start, start+childNodeOffet, end, cummulativeString, curStrOffset+edgeStrLen, output);	
	}
}


inline void parseTrie(const uint8_t* start, const uint8_t* end, std::vector<Entry>& output)
{
	// empty trie has no entries
	if ( start == end )
		return;
	
	char cummulativeString[4000];
	std::vector<EntryWithOffset> entries;
	processExportNode(start, start, end, cummulativeString, 0, entries);
	// to preserve tie layout order, sort by node offset
	std::sort(entries.begin(), entries.end());
	// copy to output
	output.reserve(entries.size());
	for (std::vector<EntryWithOffset>::iterator it=entries.begin(); it != entries.end(); ++it)
		output.push_back(it->entry);
}




}; // namespace trie
}; // namespace mach_o


#endif	// __MACH_O_TRIE__


