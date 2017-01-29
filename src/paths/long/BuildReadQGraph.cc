///////////////////////////////////////////////////////////////////////////////
//                   SOFTWARE COPYRIGHT NOTICE AGREEMENT                     //
//       This software and its documentation are copyright (2014) by the     //
//   Broad Institute.  All rights are reserved.  This software is supplied   //
//   without any warranty or guaranteed support whatsoever. The Broad        //
//   Institute is not responsible for its use, misuse, or functionality.     //
///////////////////////////////////////////////////////////////////////////////
/*
 * BuildReadQGraph.cc
 *
 *  Created on: Jan 22, 2014
 *      Author: tsharpe
 */

#include "paths/long/BuildReadQGraph.h"
#include "Basevector.h"
#include "FastaFileset.h"
#include "Intvector.h"
#include "IteratorRange.h"
#include "MapReduceEngine.h"
#include "PairsManager.h"
#include "ParallelVecUtilities.h"
#include "Qualvector.h"
#include "Vec.h"
#include "dna/Bases.h"
#include "feudal/BinaryStream.h"
#include "feudal/VirtualMasterVec.h"
//#include "kmers/BigKPather.h"
#include "kmers/ReadPatherDefs.h"
#include "math/Functions.h"
#include "paths/KmerBaseBroker.h"
#include "paths/UnibaseUtils.h"
#include "paths/long/HBVFromEdges.h"
#include "paths/long/KmerCount.h"
#include "system/SortInPlace.h"
#include "system/SpinLockedData.h"
#include "system/WorklistN.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>
#include <util/OutputLog.h>
#include "paths/HyperBasevector.h"
#include "paths/long/ExtendReadPath.h"
#include "paths/long/ShortKmerReadPather.h"

namespace
{
    const unsigned K = 60;
    typedef KMer<K> BRQ_Kmer;
    typedef KMer<K-1> BRQ_SubKmer;
    typedef KmerDictEntry<K> BRQ_Entry;
    typedef KmerDict<K> BRQ_Dict;

    struct __attribute__((__packed__)) KMerNodeFreq_s {
        uint64_t kdata[2];
        uint8_t count;
        uint8_t kc;
        inline const operator==(KMerNodeFreq_s const & other) const {
            //return 0==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
            return (kdata[0]==other.kdata[0] and kdata[1]==other.kdata[1]);
        }
        inline const operator<(KMerNodeFreq_s const & other) const{
            //return -1==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
            if (kdata[0]<other.kdata[0]) return true;
            if (kdata[0]==other.kdata[0] and kdata[1]<other.kdata[1]) return true;
            return false;
        }
        inline const operator>(KMerNodeFreq_s const & other) const{
            if (kdata[0]>other.kdata[0]) return true;
            if (kdata[0]==other.kdata[0] and kdata[1]>other.kdata[1]) return true;
            return false;
        }
        inline void combine(KMerNodeFreq_s const & other){
            auto newcount=count+other.count;
            if ( newcount>count) count=newcount;
            kc|=other.kc;
        }
        /*inline const operator=(KMerNodeFreq_s const & other) const {
            memcpy(&this,&other,sizeof(this));
        }*/
    };

    class KMerNodeFreq: public KMer<K>{
    public:
        KMerNodeFreq(){};
        template <class Itr>
        explicit KMerNodeFreq( Itr start )
        { assign(start,NopMapper()); }

        KMerNodeFreq (const KMerNodeFreq &other){
            *this=other;
            count=other.count;
            kc=other.kc;
        }
        KMerNodeFreq (const KMerNodeFreq_s &other) {
            memcpy (&this->mVal,&other.kdata,sizeof(KMer<K>));
            count=other.count;
            kc.mVal=other.kc;
        }

        KMerNodeFreq (const KMerNodeFreq &other, bool rc){
            *this=other;
            count=other.count;
            kc=other.kc;
            if (rc){
                this->rc();
                kc=kc.rc();
            }
        }
        void to_struct (KMerNodeFreq_s &other) const {
            memcpy (&other.kdata,&this->mVal,sizeof(KMer<K>));
            other.count=count;
            other.kc=kc.mVal;
        }
        unsigned char count;

        KMerContext kc;
    };



    inline void summarizeEntries( BRQ_Entry* e1, BRQ_Entry* e2 )
    {
        KMerContext kc;
        size_t count = 0;
        while ( e2-- != e1 )
        {
            KDef const& kDef = e2->getKDef();
            kc |= kDef.getContext();
            count += std::max(kDef.getCount(),1ul);
        }
        KDef& kDef = e1->getKDef();
        kDef.setContext(kc);
        kDef.setCount(count);
    }

    class EdgeBuilder {
    public:
        EdgeBuilder(BRQ_Dict const &dict, vecbvec *pEdges)
                : mDict(dict), mEdges(*pEdges) {}

        void buildEdge(BRQ_Entry const &entry) {
            if (isPalindrome(entry))
                make1KmerEdge(entry);
            else if (upstreamExtensionPossible(entry)) {
                if (downstreamExtensionPossible(entry))
                    return;
                extendUpstream(entry);
            } else if (downstreamExtensionPossible(entry))
                extendDownstream(entry);
            else
                make1KmerEdge(entry);
        }

        bool isEnd(BRQ_Entry const &entry) {
            if (isPalindrome(entry))
                return true;
            else if (upstreamExtensionPossible(entry) and downstreamExtensionPossible(entry))
                    return false;
            return true;
        }

        // not thread-safe
        void simpleCircle(BRQ_Entry const &entry) {
            BRQ_Entry const *pFirstEntry = &entry;
            mEdgeSeq.assign(entry.begin(), entry.end());
            mEdgeEntries.push_back(pFirstEntry);
            KMerContext context = entry.getKDef().getContext();
            BRQ_Kmer kmer(entry);
            while (true) {
                ForceAssertEq(context.getPredecessorCount(), 1u);
                ForceAssertEq(context.getSuccessorCount(), 1u);
                unsigned char succCode = context.getSingleSuccessor();
                kmer.toSuccessor(succCode);
                BRQ_Entry const *pEntry = lookup(kmer, &context);
                if (pEntry == pFirstEntry)
                    break;
                if (!pEntry->getKDef().isNull()) {
                    std::cout << "Failed to close circle.\n";
                    for (auto beg = mEdgeEntries.begin(), end = mEdgeEntries.end();
                         beg != end; ++beg)
                        std::cout << *beg << ' ' << **beg << '\n';
                    std::cout << pEntry << ' ' << *pEntry << std::endl;
                    CRD::exit(1);
                }
                mEdgeSeq.push_back(succCode);
                mEdgeEntries.push_back(pEntry);
            }
            canonicalizeCircle();
            addEdge();
        }

    private:
        void canonicalizeCircle() {
            auto itr = std::min_element(mEdgeEntries.begin(), mEdgeEntries.end(),
                                        [](BRQ_Kmer const *pEnt1, BRQ_Kmer const *pEnt2) { return *pEnt1 < *pEnt2; });
            BRQ_Kmer const &minKmer = **itr;
            size_t idx = itr - mEdgeEntries.begin();
            if (CF<K>::getForm(mEdgeSeq.begin(idx)) == CanonicalForm::REV) {
                mEdgeSeq.ReverseComplement();
                std::reverse(mEdgeEntries.begin(), mEdgeEntries.end());
                idx = mEdgeSeq.size() - idx - K;
                Assert(std::equal(minKmer.begin(), minKmer.end(), mEdgeSeq.begin(idx)));
            }
            if (!idx)
                return;
            bvec bv;
            bv.reserve(mEdgeSeq.size());
            bv.assign(mEdgeSeq.begin(idx), mEdgeSeq.end());
            bv.append(mEdgeSeq.begin(K - 1), mEdgeSeq.begin(K + idx - 1));
            Assert(std::equal(mEdgeSeq.begin(), mEdgeSeq.begin(K - 1),
                              mEdgeSeq.end() - (K - 1)));
            mEdgeSeq = bv;
            mEdgeEntries.reserve(mEdgeEntries.size() + idx);
            std::copy(mEdgeEntries.begin(), mEdgeEntries.begin() + idx,
                      std::back_inserter(mEdgeEntries));
            mEdgeEntries.erase(mEdgeEntries.begin(), mEdgeEntries.begin() + idx);
        }

        bool isPalindrome(BRQ_Kmer const &kmer) {
            if (!(K & 1))
                return kmer.isPalindrome();
            BRQ_SubKmer subKmer(kmer);
            if (subKmer.isPalindrome())
                return true;
            subKmer.toSuccessor(kmer.back());
            return subKmer.isPalindrome();
        }

        bool upstreamExtensionPossible(BRQ_Entry const &entry) {
            KMerContext context = entry.getKDef().getContext();
            if (context.getPredecessorCount() != 1)
                return false;
            BRQ_Kmer pred(entry);
            pred.toPredecessor(context.getSinglePredecessor());
            if (isPalindrome(pred))
                return false;
            lookup(pred, &context);
            return context.getSuccessorCount() == 1;
        }

        bool downstreamExtensionPossible(BRQ_Entry const &entry) {
            KMerContext context = entry.getKDef().getContext();
            if (context.getSuccessorCount() != 1)
                return false;
            BRQ_Kmer succ(entry);
            succ.toSuccessor(context.getSingleSuccessor());
            if (isPalindrome(succ))
                return false;
            lookup(succ, &context);
            return context.getPredecessorCount() == 1;
        }

        void make1KmerEdge(BRQ_Entry const &entry) {
            mEdgeSeq.assign(entry.begin(), entry.end());
            mEdgeEntries.push_back(&entry);
            addEdge();
        }

        void extendUpstream(BRQ_Entry const &entry) {
            mEdgeSeq.assign(entry.rcbegin(), entry.rcend());
            mEdgeEntries.push_back(&entry);
            extend(BRQ_Kmer(entry).rc(), entry.getKDef().getContext().rc());
        }

        void extendDownstream(BRQ_Entry const &entry) {
            mEdgeSeq.assign(entry.begin(), entry.end());
            mEdgeEntries.push_back(&entry);
            extend(entry, entry.getKDef().getContext());
        }

        void extend(BRQ_Kmer const &kmer, KMerContext context) {
            BRQ_Kmer next(kmer);
            while (context.getSuccessorCount() == 1) {
                unsigned char succCode = context.getSingleSuccessor();
                next.toSuccessor(succCode);
                if (isPalindrome(next))
                    break;
                BRQ_Entry const *pEntry = lookup(next, &context);
                if (context.getPredecessorCount() != 1)
                    break;
                mEdgeSeq.push_back(succCode);
                mEdgeEntries.push_back(pEntry);
            }
            switch (mEdgeSeq.getCanonicalForm()) {
                case CanonicalForm::PALINDROME:
                    ForceAssertEq(mEdgeSeq.size(), K);
                    // allow flow-through to FWD case
                case CanonicalForm::FWD:
                    addEdge();
                    break;
                case CanonicalForm::REV:
                    mEdgeSeq.clear();
                    mEdgeEntries.clear();
                    break;
            }
        }

        BRQ_Entry const *lookup(BRQ_Kmer const &kmer, KMerContext *pContext) {
            BRQ_Entry const *result;
            if (kmer.isRev()) {
                result = mDict.findEntryCanonical(BRQ_Kmer(kmer).rc());
                ForceAssert(result);
                *pContext = result->getKDef().getContext().rc();
            } else {
                result = mDict.findEntryCanonical(kmer);
                ForceAssert(result);
                *pContext = result->getKDef().getContext();
            }
            return result;
        }

        void addEdge() {
            static SpinLockedData gLock;
            EdgeID edgeID;
            if (mEdgeSeq.getCanonicalForm() == CanonicalForm::REV) {
                mEdgeSeq.ReverseComplement();
                std::reverse(mEdgeEntries.begin(), mEdgeEntries.end());
            }
            if (true) {
                SpinLocker lock(gLock);
                edgeID.setVal(mEdges.size());
                mEdges.push_back(mEdgeSeq);
            }
            unsigned offset = 0;
            bool err = false;
            for (BRQ_Entry const *pEnt : mEdgeEntries) {
                KDef const &kDef = pEnt->getKDef();
                if (kDef.isNull()) {
                    Assert(std::equal(pEnt->begin(), pEnt->end(), mEdgeSeq.begin(offset)) ||
                           std::equal(pEnt->rcbegin(), pEnt->rcend(), mEdgeSeq.begin(offset)));
                    const_cast<KDef &>(kDef).set(edgeID, offset++);
                } else {
                    std::cout << edgeID << ':' << offset++ << ' ' << pEnt
                              << " Already occupied as " << kDef.getEdgeID()
                              << ':' << kDef.getEdgeOffset() << std::endl;
                    err = true;
                }
            }
            if (err)
                FatalErr("Having trouble with preoccupied kmers.");
            mEdgeSeq.clear();
            mEdgeEntries.clear();
        }

        BRQ_Dict const &mDict;
        vecbvec &mEdges;
        std::vector<BRQ_Entry const *> mEdgeEntries;
        bvec mEdgeSeq;
    };

    void buildEdges( BRQ_Dict const& dict, vecbvec* pEdges )
    {
        EdgeBuilder eb(dict,pEdges);
        dict.parallelForEachHHS(
                [eb]( BRQ_Dict::Set::HHS const& hhs ) mutable
                { for ( BRQ_Entry const& entry : hhs )
                    if ( entry.getKDef().isNull() )
                        eb.buildEdge(entry); });

        size_t nRegularEdges = pEdges->size();
        size_t nTotalLength = pEdges->SizeSum();
        OutputLog(2) << nRegularEdges
                  << " edges of total length " << nTotalLength << std::endl;

        // if a kmer isn't marked as being on an edge, it must be a part of a smooth
        // circle: add those edges, too.  simpleCircle method isn't thread-safe, so
        // this part is single-threaded.
        //std::cout << Date() << ": finding smooth circles." << std::endl;
        for ( auto const& hhs : dict )
            for ( auto const& entry : hhs )
                if ( entry.getKDef().isNull() )
                    eb.simpleCircle(entry);
        OutputLog(2) << pEdges->size()-nRegularEdges
                  << " circular edges of total length "
                  << pEdges->SizeSum()-nTotalLength << std::endl;
    }

    template <class Itr1, class Itr2>
    size_t matchLen( Itr1 itr1, Itr1 end1, Itr2 itr2, Itr2 end2 )
    {
        size_t result = 0;

        while ( itr1 != end1 && itr2 != end2 && *itr1 == *itr2 )
        { ++result; ++itr1; ++itr2; }

        return result;
    }


    template <class Itr1, class Itr2, class ItrW, typename sumType >
    size_t fuzzyMatchLen( Itr1 itr1, Itr1 end1, Itr2 itr2, Itr2 end2,
                          ItrW itrw, ItrW endw, sumType& maxWeight)
    {
        size_t result = 0;

        while ( itr1 != end1 && itr2 != end2 && itrw != endw )
        {
            if ( *itr1 != *itr2 ) maxWeight -= *itrw;

            if ( maxWeight < 0 ) break;             // EARLY EXIT!

            ++result; ++itr1; ++itr2; ++itrw;
        }

        return result;
    }

    class EdgeLoc
    {
    public:
        EdgeLoc() : mEdgeOffset(0) {}
        EdgeLoc( EdgeID const& edgeID, bool rc, unsigned edgeOffset )
                : mEdgeID(edgeID), mEdgeOffset(edgeOffset)
        { if ( rc ) mEdgeOffset = ~edgeOffset; }

        EdgeID const& getEdgeID() const { return mEdgeID; }
        bool isRC() const { return mEdgeOffset < 0; }
        unsigned getEdgeOffset() const
        { return mEdgeOffset < 0 ? ~mEdgeOffset : mEdgeOffset; }

        size_t hash() const { return (mEdgeID.val() << 32) | mEdgeOffset; }

        friend bool operator<( EdgeLoc const& el1, EdgeLoc const& el2 )
        { if ( el1.mEdgeID < el2.mEdgeID ) return true;
            if ( el2.mEdgeID < el1.mEdgeID ) return false;
            return el1.mEdgeOffset < el2.mEdgeOffset; }

        friend std::ostream& operator<<( std::ostream& os, EdgeLoc const& el )
        { if ( el.isRC() ) os << '~';
            return os << el.getEdgeID() << '[' << el.getEdgeOffset() << ']'; }

    private:
        EdgeID mEdgeID;
        int mEdgeOffset;
    };

    class PathPart
    {
    public:
        PathPart() : mLen(0u), mEdgeLen(0) {}
        PathPart( unsigned gapLen ) : mLen(gapLen), mEdgeLen(0) {}
        PathPart( EdgeID const& edgeID, bool rc, unsigned edgeOffset,
                  unsigned len, unsigned edgeLen )
                : mEdgeLoc(edgeID,rc,edgeOffset), mLen(len), mEdgeLen(edgeLen)
        { AssertLt(getEdgeOffset(),mEdgeLen); }

        EdgeID const& getEdgeID() const { return mEdgeLoc.getEdgeID(); }
        bool isRC() const { return mEdgeLoc.isRC(); }
        unsigned getEdgeOffset() const { return mEdgeLoc.getEdgeOffset(); }

        // if true, then only the length is meaningful
        bool isGap() const { return !mEdgeLen; }
        unsigned getLength() const { return mLen; }
        void incrLength(unsigned len) { mLen += len; }

        unsigned getEndOffset() const { return getEdgeOffset()+mLen; }

        // note: edge length is measured in kmers, not bases
        unsigned getEdgeLen() const { return mEdgeLen; }

        EdgeLoc firstLoc() const
        { return mEdgeLoc; }

        EdgeLoc lastLoc() const
        { EdgeLoc el(getEdgeID(),isRC(),getEdgeOffset()+mLen-1);
            AssertLt(el.getEdgeOffset(),mEdgeLen);
            return el; }

        bool isSameEdge( PathPart const& that ) const
        { return getEdgeID() == that.getEdgeID() && isRC() == that.isRC(); }

        bool isConformingCapturedGap( unsigned maxJitter ) const
        { PathPart const& prev = this[-1];
            PathPart const& next = this[1];
            unsigned graphDist = next.getEdgeOffset()-prev.getEndOffset();
            if ( !prev.isSameEdge(next) )
                graphDist += prev.getEdgeLen();
            // if the gap size is consistent with the graph, return true
            return unsigned(std::abs(int(mLen-graphDist))) <= maxJitter; }

        PathPart rc() const
        { return isGap() ? PathPart(mLen) :
                 PathPart(getEdgeID(),!isRC(),mEdgeLen-getEndOffset(),mLen,mEdgeLen); }

        friend std::ostream& operator<<( std::ostream& os, PathPart const& ppt )
        { if ( ppt.isGap() ) return os << ppt.getLength() << 'X';
            if ( ppt.isRC() ) os << '~';
            int offset = ppt.getEdgeOffset();
            return os << ppt.getEdgeID()
                   << '[' << offset << '-' << offset+ppt.getLength()
                   << ':' << ppt.getEdgeLen() << ']'; }

    private:
        EdgeLoc mEdgeLoc;
        unsigned mLen;
        unsigned mEdgeLen;
    };

    class BRQ_Pather
    {
    public:
        BRQ_Pather( BRQ_Dict const& dict, vecbvec const& edges )
                : mDict(dict), mEdges(edges) {}

        std::vector<PathPart> const& path( bvec const& read )
        {
            mPathParts.clear();
            if (read.size() < K) {
                mPathParts.emplace_back(read.size());
                return mPathParts;
            } // EARLY RETURN!

            auto itr = read.begin();
            auto end = read.end() - K + 1;
            while (itr != end) {
                BRQ_Kmer kmer(itr);
                BRQ_Entry const *pEnt = mDict.findEntry(kmer);
                if (!pEnt) {
                    unsigned gapLen = 1u;
                    auto itr2 = itr + K;
                    ++itr;
                    auto end2 = read.end();
                    while (itr2 != end2) {
                        kmer.toSuccessor(*itr2);
                        ++itr2;
                        if ((pEnt = mDict.findEntry(kmer)))
                            break;
                        ++gapLen;
                        ++itr;
                    }
                    mPathParts.emplace_back(gapLen);
                }
                if (pEnt) {
                    KDef const &kDef = pEnt->getKDef();
                    bvec const &edge = mEdges[kDef.getEdgeID().val()];
                    int offset = kDef.getEdgeOffset();
                    auto eBeg(edge.begin(offset));
                    size_t len = 1u;
                    bool rc = CF<K>::isRC(itr, eBeg);
                    if (!rc)
                        len += matchLen(itr + K, read.end(), eBeg + K, edge.end());
                    else {
                        offset = edge.size() - offset;
                        auto eBegRC(edge.rcbegin(offset));
                        len += matchLen(itr + K, read.end(), eBegRC, edge.rcend());
                        offset = offset - K;
                    }
                    unsigned edgeKmers = edge.size() - K + 1;

                    mPathParts.emplace_back(kDef.getEdgeID(), rc, offset, len, edgeKmers);
                    itr += len;
                }
            }
            return mPathParts;
        }

        bool isJoinable( PathPart const& pp1, PathPart const& pp2 ) const
        { if ( pp1.getEdgeID() == pp2.getEdgeID() ) return true;
            bvec const& e1 = mEdges[pp1.getEdgeID().val()];
            bvec const& e2 = mEdges[pp2.getEdgeID().val()];
            BRQ_SubKmer k1 = pp1.isRC() ? BRQ_SubKmer(e1.rcend()-K+1) : BRQ_SubKmer(e1.end()-K+1);
            BRQ_SubKmer k2 = pp2.isRC() ? BRQ_SubKmer(e2.rcend()-K+1) : BRQ_SubKmer(e2.end()-K+1);
            return k1==k2; }

    private:
        BRQ_Dict const& mDict;
        vecbvec const& mEdges;
        std::vector<PathPart> mPathParts;
    };

    class GapFiller
    {
    public:
        GapFiller( vecbvec const& reads, vecbvec const& edges, unsigned maxGapSize,
                   unsigned minFreq, BRQ_Dict* pDict )
                : mReads(reads), mDict(*pDict), mPather(*pDict,edges),
                  mMaxGapSize(maxGapSize), mMinFreq(minFreq) {}

        template <class OItr>
        void map( size_t readId, OItr oItr )
        { bvec const& read = mReads[readId];
            std::vector<PathPart> const& parts = mPather.path(read);
            if ( parts.size() < 3 ) return; // EARLY RETURN!
            auto rItr = read.begin(parts[0].getLength());
            auto end = parts.end()-1;
            // for each path part sandwiched between two others
            for ( auto pPart=parts.begin()+1; pPart != end; ++pPart )
            { // if it's not a gap of appropriate size
                if ( !pPart->isGap() ||
                     (mMaxGapSize && pPart->getLength() > mMaxGapSize) ||
                     // or if it fits cleanly on the existing graph
                     pPart->isConformingCapturedGap(MAX_JITTER) )
                { rItr += pPart->getLength();
                    continue; } // just CONTINUE
                // we have a gap of appropriate size, and it has new, interesting kmers
                // that might change the graph structure
                auto itr = rItr - 1;
                BRQ_Kmer kkk(itr); itr += K;
                update(kkk,KMerContext::initialContext(*itr));
                auto last = itr + pPart->getLength();
                while ( itr != last )
                { unsigned char predCode = kkk.front();
                    kkk.toSuccessor(*itr);
                    KMerContext kc(predCode,*++itr);
                    *oItr++ = kkk.isRev()?BRQ_Entry(BRQ_Kmer(kkk).rc(),kc.rc()) : BRQ_Entry(kkk,kc); }
                KMerContext kc = KMerContext::finalContext(kkk.front());
                kkk.toSuccessor(*itr);
                update(kkk,kc);
                rItr += pPart->getLength(); } }

        void reduce( BRQ_Entry* e1, BRQ_Entry* e2 )
        { summarizeEntries(e1,e2);
            if ( e1->getKDef().getCount() >= mMinFreq )
            { mDict.insertEntry(std::move(*e1)); } }

        BRQ_Entry* overflow( BRQ_Entry* e1, BRQ_Entry* e2 )
        { if ( e2-e1 > 1 ) summarizeEntries(e1,e2); return e1+1; }

    private:
        void update( BRQ_Kmer kmer, KMerContext kc )
        { if ( kmer.isRev() ) { kmer.rc(); kc=kc.rc(); }
            mDict.applyCanonical(kmer,
                                 [kc](BRQ_Entry const& ent)
                                 { KDef& kd = const_cast<KDef&>(ent.getKDef());
                                     kd.setContext(kc|kd.getContext()); }); }

        static unsigned const MAX_JITTER = 1;
        vecbvec const& mReads;
        BRQ_Dict& mDict;
        BRQ_Pather mPather;
        unsigned mMaxGapSize;
        unsigned mMinFreq;
    };
    typedef MapReduceEngine<GapFiller,BRQ_Entry,BRQ_Kmer::Hasher> GFMRE;

    void fillGaps( vecbvec const& reads, unsigned maxGapSize, unsigned minFreq,
                   vecbvec* pEdges, BRQ_Dict* pDict )
    {
        //std::cout << Date() << ": filling gaps." << std::endl;
        GapFiller gf(reads,*pEdges,maxGapSize,minFreq,pDict);
        GFMRE mre(gf);
        if ( !mre.run(5*reads.size(),0ul,reads.size()) )
            FatalErr("Map/Reduce operation failed in gap filling.");

        //std::cout << "Now the dictionary has " << pDict->size()
        //                << " entries." << std::endl;

        //std::cout << Date() << ": finding edge sequences again." << std::endl;
        pDict->nullEntries();
        pDict->recomputeAdjacencies();
        pEdges->clear();
        buildEdges(*pDict,pEdges);
    }

    class BRQ_Join
    {
    public:
        BRQ_Join( EdgeLoc const& edgeLoc1, EdgeLoc const& edgeLoc2, unsigned overlap )
                : mEdgeLoc1(edgeLoc1), mEdgeLoc2(edgeLoc2), mOverlap(overlap)
        {}

        EdgeLoc const& getEdgeLoc1() const { return mEdgeLoc1; }
        EdgeLoc const& getEdgeLoc2() const { return mEdgeLoc2; }
        unsigned getOverlap() const { return mOverlap; }

        size_t hash() const
        { return 47 * (47*mEdgeLoc1.hash()+mEdgeLoc2.hash()) + mOverlap; }

        struct Hasher
        { size_t operator()( BRQ_Join const& join ) { return join.hash(); } };

        friend bool operator<( BRQ_Join const& join1, BRQ_Join const& join2 )
        { if ( join1.mEdgeLoc1 < join2.mEdgeLoc1 ) return true;
            if ( join2.mEdgeLoc1 < join1.mEdgeLoc1 ) return false;
            if ( join1.mEdgeLoc2 < join2.mEdgeLoc2 ) return true;
            if ( join2.mEdgeLoc2 < join1.mEdgeLoc2 ) return false;
            return join1.mOverlap < join2.mOverlap; }

        friend std::ostream& operator<<( std::ostream& os, BRQ_Join const& join )
        { return os << join.mEdgeLoc1 << "<-" << join.mOverlap << "->"
                 << join.mEdgeLoc2; }

    private:
        EdgeLoc mEdgeLoc1;
        EdgeLoc mEdgeLoc2;
        unsigned mOverlap;
    };

    class BRQ_Joiner
    {
    public:
        BRQ_Joiner( vecbvec const& reads, vecbvec const& edges, BRQ_Dict const& dict,
                unsigned maxGapSize, unsigned minFreq,
                vecbvec* pFakeReads )
                : mReads(reads), mEdges(edges), mPather(dict,edges),
                  mMaxGapSize(maxGapSize), mMinFreq(minFreq), mFakeReads(*pFakeReads)
        { ForceAssertLt(maxGapSize,K-1); }

        template <class OItr>
        void map( size_t readId, OItr oItr )
        { bvec const& read = mReads[readId];
            std::vector<PathPart> const& parts = mPather.path(read);
            if ( parts.size() < 3 ) return; // EARLY RETURN!
            auto end = parts.end()-1;
            for ( auto pPart=parts.begin()+1; pPart != end; ++pPart )
            { // if it's a captured gap of appropriate size
                if ( pPart->isGap() && pPart->getLength() <= mMaxGapSize )
                { PathPart const& prev = pPart[-1];
                    PathPart const& next = pPart[1];
                    unsigned overlap = K-pPart->getLength()-1;
                    if ( next.getEdgeID() < prev.getEdgeID() )
                        *oItr++ = BRQ_Join(next.rc().lastLoc(),prev.rc().firstLoc(),overlap);
                    else
                        *oItr++ = BRQ_Join(prev.lastLoc(),next.firstLoc(),overlap); } } }

        void reduce( BRQ_Join* pJoin1, BRQ_Join* pJoin2 )
        { Assert(validOverlap(*pJoin1));
            if ( pJoin2-pJoin1 >= mMinFreq )
            { //std::cout << "Joining: " << *pJoin1 << std::endl;
                bvec bv; bv.reserve(2*K);
                append(pJoin1->getEdgeLoc1(),0,&bv);
                append(pJoin1->getEdgeLoc2(),pJoin1->getOverlap(),&bv);
                add(bv); } }

        BRQ_Join* overflow( BRQ_Join* pJoin1, BRQ_Join* pJoin2 )
        { return pJoin1+std::min(unsigned(pJoin2-pJoin1),mMinFreq); }

    private:
        bool validOverlap( BRQ_Join const& join )
        { EdgeLoc const& el1 = join.getEdgeLoc1();
            bvec const& bv1 = mEdges[el1.getEdgeID().val()];
            EdgeLoc const& el2 = join.getEdgeLoc2();
            bvec const& bv2 = mEdges[el2.getEdgeID().val()];
            bool result;
            if ( el1.isRC() )
            { auto end = bv1.rcbegin(el1.getEdgeOffset()+K);
                auto beg = end - join.getOverlap();
                if ( el2.isRC() )
                    result = std::equal(beg,end,bv2.rcbegin(el2.getEdgeOffset()));
                else
                    result = std::equal(beg,end,bv2.begin(el2.getEdgeOffset())); }
            else
            { auto end = bv1.begin(el1.getEdgeOffset()+K);
                auto beg = end - join.getOverlap();
                if ( el2.isRC() )
                    result = std::equal(beg,end,bv2.rcbegin(el2.getEdgeOffset()));
                else
                    result = std::equal(beg,end,bv2.begin(el2.getEdgeOffset())); }
            return result; }

        void append( EdgeLoc const& el, unsigned indent, bvec* pBV )
        { bvec const& edge = mEdges[el.getEdgeID().val()];
            unsigned offset = el.getEdgeOffset()+indent;
            unsigned len = K-indent;
            if ( el.isRC() )
            { auto itr = edge.rcbegin(offset);
                pBV->append(itr,itr+len); }
            else
            { auto itr = edge.begin(offset);
                pBV->append(itr,itr+len); } }

        void add( bvec const& bv )
        { static SpinLockedData gLock;
            if ( true )
            { SpinLocker lock(gLock);
                mFakeReads.push_back(bv); } }

        vecbvec const& mReads;
        vecbvec const& mEdges;
        BRQ_Pather mPather;
        unsigned mMaxGapSize;
        unsigned mMinFreq;
        vecbvec& mFakeReads;
    };
    typedef MapReduceEngine<BRQ_Joiner,BRQ_Join,BRQ_Join::Hasher> JMRE;

    void joinOverlaps( vecbvec const& reads, unsigned maxGapSize, unsigned minFreq,
                       vecbvec* pEdges, BRQ_Dict* pDict )
    {
        //std::cout << Date() << ": joining overlaps." << std::endl;
        vecbvec fakeReads;
        fakeReads.reserve(pEdges->size()/10);
        BRQ_Joiner joiner(reads,*pEdges,*pDict,maxGapSize,minFreq,&fakeReads);
        JMRE mre(joiner);
        if ( !mre.run(reads.size(),0ul,reads.size()) )
            FatalErr("Map/Reduce operation failed when joining overlaps.");

        if ( fakeReads.size() )
        {
            //std::cout << "Found " << fakeReads.size() << " joins." << std::endl;
            pDict->nullEntries();
            pDict->process(fakeReads);
            //std::cout << "Now the dictionary has " << pDict->size()
            //                    << " entries." << std::endl;
            //std::cout << Date() << ": finding edge sequences again." << std::endl;
            pEdges->clear();
            buildEdges(*pDict,pEdges);
        }
    }


    inline size_t pathPartToEdgeID( PathPart const& part, std::vector<int> const& mFwdEdgeXlat, std::vector<int> const& mRevEdgeXlat )
    {
        ForceAssert(!part.isGap());
        size_t idx = part.getEdgeID().val();
        return ( part.isRC() ? mRevEdgeXlat[idx] : mFwdEdgeXlat[idx] );
    }

    inline void pathPartsToReadPath( std::vector<PathPart> const& parts, ReadPath& path, std::vector<int> const& mFwdEdgeXlat, std::vector<int> const& mRevEdgeXlat )
    {
        path.clear();
        PathPart const *pLast = 0;
        for (PathPart const &part : parts) {
            if (part.isGap()) continue;
            if (pLast && pLast->isSameEdge(part)) continue;
//        if ( part.getEdgeOffset() == 0 && part.getLength() < 5) continue;
            size_t idx = part.getEdgeID().val();
            path.push_back(part.isRC() ? mRevEdgeXlat[idx] : mFwdEdgeXlat[idx]);
            pLast = &part;
        }
        if (path.empty()) { path.setOffset(0); /*path.setLastSkip(0);*/ }
        else {
            PathPart const &firstPart = parts.front();
            if (!firstPart.isGap())
                path.setOffset(firstPart.getEdgeOffset());
            else {
                int eo1 = parts[1].getEdgeOffset();
                int eo2 = firstPart.getLength();
                path.setOffset(eo1 - eo2);
            }
        }
    }

    void path_reads_OMP( vecbvec const& reads, VecPQVec const& quals, BRQ_Dict const& dict, vecbvec const& edges,
            HyperBasevector const& hbv, std::vector<int> const& fwdEdgeXlat, std::vector<int> const& revEdgeXlat,
                     ReadPathVec* pPaths) {
        static unsigned const MAX_JITTER = 3;
        #pragma omp parallel
        {

            vec<int> toLeft,toRight;
            hbv.ToLeft(toLeft);
            hbv.ToLeft(toRight);
            BRQ_Pather mPather(dict,edges);
            ReadPath mPath;
            ExtendReadPath mExtender(hbv,&toLeft,&toRight);
            qvec mQV;

            #pragma omp for
            for (size_t readId=0;readId<reads.size();++readId){
                std::vector<PathPart> parts = mPather.path(reads[readId]);     // needs to become a forward_list

                // convert any seeds on hanging edges to gaps
                std::vector<PathPart> new_parts;
                for ( auto part : parts ) {
                    if ( !part.isGap() ) {
                        size_t edge_id = pathPartToEdgeID(part,fwdEdgeXlat,revEdgeXlat);
                        size_t vleft  = toLeft[edge_id];
                        size_t vright = toRight[edge_id];
                        if ( hbv.ToSize(vleft) == 0
                             && hbv.ToSize(vright) > 1
                             && hbv.FromSize(vright) > 0
                             && part.getEdgeLen() <= 100 ) {
                            // delete a seed on a hanging edge
                            part = PathPart(part.getLength() );
                        }
                    }

                    // either add to an existing gap or create a new part (gap or not)
                    if ( part.isGap() && new_parts.size() && new_parts.back().isGap() )
                        new_parts.back().incrLength( part.getLength() );
                    else
                        new_parts.push_back(part);
                }
                std::swap(parts,new_parts);

                // if a gap is captured between two edges and things don't make sense, delete the
                // seed, if there are more than one, and subsequent seeds.  This hopefully avoids
                // the loss of sensitivity of just dropping the seeds.
                if ( parts.size() >= 3 ) {
                    size_t seeds = ( parts.begin()->isGap()) ? 0u : 1u;
                    for ( auto pPart = parts.begin()+1; pPart != parts.end()-1; ++pPart ) {
                        if ( !pPart->isGap() ) { seeds++; continue; }
                        if ( !pPart->isConformingCapturedGap(MAX_JITTER) ||
                             !mPather.isJoinable(pPart[-1],pPart[1]) ) {


                            if ( seeds > 1 ) {
                                PathPart tmpPart(pPart[-1].getLength());
                                for ( auto pPart2 = pPart; pPart2 != parts.end(); ++ pPart2 )
                                    tmpPart.incrLength( pPart2->getLength() );
                                parts.erase( pPart-1, parts.end() );
                                parts.push_back(tmpPart);
                            } else {
                                for ( auto pPart2 = pPart+1; pPart2 != parts.end(); ++ pPart2 )
                                    pPart->incrLength( pPart2->getLength() );
                                parts.erase( pPart+1, parts.end() );
                            }

                            break;
                        }
                    }
                }


                // if a seed has only taken us <= 5 kmers onto an edge, we back off
                // because we probably don't have enough evidence to commit.  Extension
                // code later will be more careful.
                if ( parts.back().isGap() && parts.size() > 1 ) {
                    auto const& last2 = parts[parts.size()-2];
                    if ( last2.getEdgeOffset() == 0 && last2.getLength() <= 5 ) {
                        auto last = parts.back();
                        last.incrLength(last2.getLength());
                        parts.pop_back();
                        parts.pop_back();
                        parts.push_back(last);
                    }
                } else if ( !parts.back().isGap() ) {
                    auto& last = parts.back();
                    if ( last.getEdgeOffset()==0 && last.getLength() <= 5 )  {
                        last = PathPart( last.getLength() );
                    }
                }

                pathPartsToReadPath(parts, mPath,fwdEdgeXlat,revEdgeXlat);

                quals[readId].unpack(&mQV);
                mExtender.attemptLeftRightExtension(mPath,reads[readId],mQV);

                (*pPaths)[readId] = mPath;
            }
        }

    }

} // end of anonymous namespace


inline void combine_Entries( BRQ_Entry & dest, BRQ_Entry & source )
{
    KDef& kDef = dest.getKDef();
    KMerContext kc=dest.getKDef().getContext();
    kc|=source.getKDef().getContext();
    kDef.setContext(kc);
    kDef.setCount(kDef.getCount()+source.getKDef().getCount());
}

inline void combine_Entries( KMerNodeFreq & dest, KMerNodeFreq & source )
{
    dest.kc|=source.kc;
    uint16_t c = dest.count;
    c+=source.count;
    dest.count= (c<255 ? c:255);
}


void print_results_status(uint nthreads, bool results[]){
#pragma omp critical
    {
        std::cout<< "Results status: [ ";
        for (auto i=0;i<nthreads;++i) std::cout<<results[i]<<" ";
        std::cout<<" ]"<<std::endl;
    }
}

void create_read_lengths(std::vector<uint16_t> & rlen, VecPQVec const& quals, unsigned minQual){

    uint64_t qsize=quals.size();
    rlen.resize(qsize);
    OutputLog(2) << "Creating read lengths for "<<qsize<<" reads at quality >= " << minQual << std::endl;
    #pragma omp parallel shared(rlen,quals)
    {
        qvec uq;
        #pragma omp for
        for (auto i = 0; i < qsize; ++i) {
            quals[i].unpack(&uq);
            auto itr = uq.end();
            auto beg = uq.begin();
            uint16_t good = 0;
            while (itr != beg) {
                if (*--itr < minQual) good = 0;
                else if (++good == K) {
                    rlen[i] = (itr - beg) + K;
                    break;
                }
            }
        }
    }
    OutputLog(2) << "Read lengths created"<<std::endl;
}
uint64_t count_good_lengths(std::vector<uint16_t> &good_lenghts, VecPQVec const& quals, uint64_t from, uint64_t to, unsigned _K, unsigned minQual){
    //Computes the length in _K-mers til hitting minQual on each qual[from-to], returns the total count of goof kmers
    uint64_t nKmers=0;
    qvec uq;
    auto itr = uq.end();
    auto beg = uq.begin();
    unsigned good = 0;

    for (auto i = from; i < to; ++i) {
        quals[i].unpack(&uq);
        itr = uq.end();
        beg = uq.begin();
        good = 0;
        while (itr != beg) {
            if (*--itr < minQual) good = 0;
            else if (++good == _K) {
                good_lenghts[i-from] = (itr - beg) + _K;
                break;
            }
        }
        nKmers += good_lenghts[i-from];

    }
    return nKmers;

}

void collapse_entries(std::vector<BRQ_Entry> &kmer_list){
    auto okItr = kmer_list.begin();
    for (auto kItr = kmer_list.begin(); kItr < kmer_list.end(); ++okItr) {
        *okItr = *kItr;
        ++kItr;
        while (kItr<kmer_list.end() and *okItr == *kItr) {
            combine_Entries(*okItr,*kItr);
            ++kItr;
        }
    }
    kmer_list.resize(okItr - kmer_list.begin());
}

void collapse_entries(std::vector<KMerNodeFreq> &kmer_list){
    auto okItr = kmer_list.begin();
    for (auto kItr = kmer_list.begin(); kItr < kmer_list.end(); ++okItr) {
        *okItr = *kItr;
        ++kItr;
        while (kItr<kmer_list.end() and *okItr == *kItr) {
            combine_Entries(*okItr,*kItr);
            ++kItr;
        }
    }
    kmer_list.resize(okItr - kmer_list.begin());
}

void collapse_entries(std::vector<KMerNodeFreq_s> &kmer_list){
    auto okItr = kmer_list.begin();
    for (auto kItr = kmer_list.begin(); kItr < kmer_list.end(); ++okItr) {
        *okItr = *kItr;
        ++kItr;
        while (kItr<kmer_list.end() and *okItr == *kItr) {
            okItr->combine(*kItr);
            ++kItr;
        }
    }
    kmer_list.resize(okItr - kmer_list.begin());
}

void inplace_count_merge(std::vector<KMerNodeFreq> & counts1, std::vector<KMerNodeFreq> & counts2){
    //sync-merge values from counts2 into counts1, if value not int counts1, move it to a top-based position in counts2.
    //std::cout<<"merging counts1("<<counts1.size()<<" kmers) and counts2("<<counts2.size()<<" kmers)"<<std::endl;
    auto itr1=counts1.begin(),end1=counts1.end();
    auto itr2=counts2.begin(),end2=counts2.end(),itr2w=counts2.begin();
    //std::cout<<"merging, accumulating on counts1"<<std::endl;
    while (itr2 !=end2){
        while (itr1!=end1 and *itr1<*itr2) ++itr1;
        if (itr1!=end1 and *itr1==*itr2){
            combine_Entries(*itr1,*itr2);
            ++itr1;++itr2;
        }
        while (itr2!=end2 and (itr1==end1 or *itr2<*itr1)){
            *itr2w=*itr2;
            ++itr2w;++itr2;
        }
    }
    //shrink counts2 to the size of its remaining elements
    //std::cout<<"merging, resizing counts2 to " << itr2w-counts2.begin() <<std::endl;
    counts2.resize(itr2w-counts2.begin());
    counts2.shrink_to_fit();
    //expand counts1 to allow the insertion of the unique values on counts2
    //std::cout<<"merging, resizing counts1 to " << counts1.size()+counts2.size() << std::endl;
    counts1.resize(counts1.size()+counts2.size());
    //merge-sort from the bottom into count1.
    //std::cout<<"merging, final merging"<<std::endl;
    auto writr1=counts1.rbegin(), ritr1=counts1.rbegin()+counts2.size(), rend1=counts1.rend();
    auto ritr2=counts2.rbegin(), rend2=counts2.rend();
    while (writr1!=rend1){
        if (ritr2!=rend2 and *ritr2>*ritr1){
            *writr1=*ritr2;
            ++ritr2;
        } else {
            *writr1=*ritr1;
            ++ritr1;
        }
        ++writr1;
    }
    counts2.clear();
    counts2.shrink_to_fit();
}

void inplace_count_merge(std::shared_ptr<std::vector<KMerNodeFreq_s>> counts1, std::shared_ptr<std::vector<KMerNodeFreq_s>> counts2){
    //sync-merge values from counts2 into counts1, if value not int counts1, move it to a top-based position in counts2.
    //std::cout<<"merging counts1("<<counts1.size()<<" kmers) and counts2("<<counts2.size()<<" kmers)"<<std::endl;
    //std::cout<<"merging"<<std::endl;
    auto itr1=counts1->begin(),end1=counts1->end();
    auto itr2=counts2->begin(),end2=counts2->end(),itr2w=counts2->begin();
    //std::cout<<"merging, accumulating on counts1"<<std::endl;
    while (itr2 !=end2){
        while (itr1!=end1 and *itr1<*itr2) ++itr1;
        if (itr1!=end1 and *itr1==*itr2){
            //combine_Entries(*itr1,*itr2);
            itr1->combine(*itr2);
            ++itr1;++itr2;
        }
        while (itr2!=end2 and (itr1==end1 or *itr2<*itr1)){
            *itr2w=*itr2;
            ++itr2w;++itr2;
        }
    }
    //shrink counts2 to the size of its remaining elements
    //std::cout<<"merging, resizing counts2 to " << itr2w-counts2.begin() <<std::endl;
    counts2->resize(itr2w-counts2->begin());
    counts2->shrink_to_fit();
    //expand counts1 to allow the insertion of the unique values on counts2
    //std::cout<<"merging, resizing counts1 to " << counts1.size()+counts2.size() << std::endl;
    counts1->resize(counts1->size()+counts2->size());
    //merge-sort from the bottom into count1.
    //std::cout<<"merging, final merging"<<std::endl;
    auto writr1=counts1->rbegin(), ritr1=counts1->rbegin()+counts2->size(), rend1=counts1->rend();
    auto ritr2=counts2->rbegin(), rend2=counts2->rend();
    while (writr1!=rend1){
        if (ritr2!=rend2 and (ritr1==rend1 or *ritr2>*ritr1)){
            memcpy(&(*writr1),&(*ritr2), sizeof(KMerNodeFreq_s));
            ++ritr2;
        } else {
            memcpy(&(*writr1),&(*ritr1), sizeof(KMerNodeFreq_s));
            ++ritr1;
        }
        ++writr1;
    }
    counts2->clear();
    counts2->shrink_to_fit();
}


std::vector<KMerNodeFreq> createDictOMPRecursive(BRQ_Dict ** dict, vecbvec const& reads, std::vector<uint16_t> const &rlen, uint64_t from, uint64_t to, uint64_t batch_size, unsigned minFreq, std::string workdir=""){
    std::vector<KMerNodeFreq> kmer_list;
    //If size larger than batch (or still not enough cpus used, or whatever), Lauch 2 tasks to sort the 2 halves, with minFreq=0
    if (to - from > batch_size) {
        uint64_t mid_point = from + (to - from) / 2;
        #pragma omp task shared(reads,rlen,kmer_list)
        { kmer_list = createDictOMPRecursive(NULL, reads, rlen, from, mid_point, batch_size, minFreq);}
        std::vector<KMerNodeFreq> entries2;
        #pragma omp task shared(reads,rlen,entries2)
        { entries2 = createDictOMPRecursive(NULL, reads, rlen, mid_point, to, batch_size, minFreq); }
        #pragma omp taskwait
        inplace_count_merge(kmer_list,entries2);

    } else { //just do the kmer creation and sort/collapse
        //#pragma omp critical
        //std::cout << "createDictOMPRecursive (thread "<<omp_get_thread_num()<<") from " << from << " to " << to << ", counting..." << std::endl;
        uint64_t total_good_lenght = std::accumulate(rlen.begin()+from,rlen.begin()+to,0);
        kmer_list.clear();
        kmer_list.reserve(total_good_lenght);
        //Populate the kmer list
        for (auto readId = from; readId < to; ++readId) {
            unsigned len = rlen[readId];
            if (len > K) {
                auto beg = reads[readId].begin(), itr=beg+K, last=beg+(len-1);
                KMerNodeFreq kkk(beg);
                kkk.kc = KMerContext::initialContext(*itr);
                kkk.count=1;
                kmer_list.push_back( kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk);
                while ( itr != last )
                { unsigned char pred = kkk.front();
                    kkk.toSuccessor(*itr); ++itr;
                    kkk.kc = KMerContext(pred,*itr);
                    kmer_list.push_back( kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk);
                }
                kkk.kc = KMerContext::finalContext(kkk.front());
                kkk.toSuccessor(*last);
                kmer_list.push_back( kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk);
            }
        }
        std::sort(kmer_list.begin(), kmer_list.end());
        collapse_entries(kmer_list);
    }
    //sort
    //#pragma omp critical
    //std::cout << "createDictOMPRecursive (thread "<<omp_get_thread_num()<<") from " << from << " to " << to << ", sorting/collapsing "<<kmer_list.size()<<" kmers ..."
    //          << std::endl;


    if (NULL != dict) { //merge sort and return that
        OutputLog(2) << kmer_list.size() << " kmers counted, filtering..." << std::endl;
        (*dict) = new BRQ_Dict(kmer_list.size());
        uint64_t used = 0,not_used=0;
        uint64_t hist[256];
        for (auto &h:hist) h=0;
        for (auto &knf:kmer_list) {
            ++hist[std::min(255,(int)knf.count)];
            if (knf.count >= minFreq) {
                (*dict)->insertEntryNoLocking(BRQ_Entry((BRQ_Kmer)knf,knf.kc));
                used++;
            } else {
                not_used++;
            }

        }
        OutputLog(2) << used << " / " << kmer_list.size() << " kmers with Freq >= " << minFreq << std::endl;
        kmer_list.clear();
        if (""!=workdir) {
            std::ofstream kff(workdir + "/small_K.freqs");
            for (auto i = 1; i < 256; i++) kff << i << ", " << hist[i] << std::endl;
            kff.close();
        }

    }
    return kmer_list;

}

void createDictOMP(BRQ_Dict ** dict, vecbvec const& reads, std::vector<uint16_t> const &rlen,
                                        uint64_t batch_size, unsigned minFreq, std::string workdir=""){

    //Compute how many "batches" will be used. and malloc a structure for them and a bool array to mark them "ready to process".
    //optionally, there could be a total count of "ready kmers" to set a limit for memory usage, if total count is too high, slaves would wait
    const uint64_t batches=(rlen.size()+batch_size-1)/batch_size;
    OutputLog(2)<<"kmer counting in "<<batches<<" batches of "<<batch_size<<" reads"<<std::endl;
    //OutputLog(2) << "each KMer uses " <<sizeof(KMer<K>)<<" bytes"<<std::endl;
    //OutputLog(2) << "each KMerContext uses " <<sizeof(KMerContext)<<" bytes"<<std::endl;
    //OutputLog(2) << "each KMerNodeFreq uses " <<sizeof(KMerNodeFreq)<<" bytes"<<std::endl;
    //OutputLog(2) << "each KMerNodeFreq_s uses " <<sizeof(KMerNodeFreq_s)<<" bytes"<<std::endl;


    std::vector<std::atomic_uint_fast8_t *> batch_status; //level->batch->status

    std::vector<std::vector< std::shared_ptr<std::vector<KMerNodeFreq_s>>>> batch_lists; //level->batch-> pointer to batch
    uint16_t levels=0;
    for (auto elements=batches; elements>1; elements=(elements+1)/2){
        batch_status.push_back((std::atomic_uint_fast8_t *) calloc(sizeof(std::atomic_uint_fast8_t),elements));
        batch_lists.push_back(std::vector< std::shared_ptr<std::vector<KMerNodeFreq_s> > >());
        batch_lists.back().resize(elements);
        OutputLog(2)<<"level "<< levels<<" created with "<<elements<<" elements "<<std::endl;
        ++levels;
    }
    std::atomic_uint_fast8_t level_count[levels]; //level->count
    for (auto &l:level_count)l=0;

    #pragma omp parallel shared(rlen,reads)
    {
        //Batch consumer: in-place merging into main list, delete the batch vector, mark done
        /*#pragma omp master
        {
            uint64_t done_batches=0;
            while (done_batches<batches) {
                for (auto batch = 0; batch < batches; ++batch) {
                    if (batch_status[0][batch]==1){
#pragma omp critical
                        OutputLog(4)<<"merging batch "<<batch<<std::endl;
                        inplace_count_merge(kmer_list,*batch_lists[0][batch]);

#pragma omp critical
                        OutputLog(4)<<"batch merged, main list has now "<<kmer_list.size()<<" kmers"<<std::endl;
                        batch_lists[0][batch]->clear();
                        batch_status[0][batch]++;
                        OutputLog(4)<<"batch emptied and marked"<<std::endl;
                        ++done_batches;
                    }

                }
            }

        }*/
        #pragma omp for schedule(dynamic)
        for (auto batch=0;batch<batches;++batch)
        {
            KMerNodeFreq_s knfs;
//#pragma omp critical
//            OutputLog(4)<<"counting for batch "<<batch<<std::endl;
            uint64_t from=batch*batch_size;
            uint64_t read_count= (batch<batches-1) ? batch_size : rlen.size() - batch_size * (batches-1);
            uint64_t to=from+read_count;

            std::shared_ptr<std::vector<KMerNodeFreq_s>> local_kmer_list= std::make_shared<std::vector<KMerNodeFreq_s>>(read_count);
            //do the counting, sorting, collapsing.
            uint64_t total_good_lenght = std::accumulate(rlen.begin()+from,rlen.begin()+to,0);
            local_kmer_list->reserve(total_good_lenght);
            //Populate the kmer list
            for (auto readId = from; readId < to; ++readId) {
                unsigned len = rlen[readId];
                if (len > K) {
                    auto beg = reads[readId].begin(), itr=beg+K, last=beg+(len-1);
                    KMerNodeFreq kkk(beg);
                    kkk.hash();
                    kkk.kc = KMerContext::initialContext(*itr);
                    kkk.count=1;
                    (kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk).to_struct(knfs);
                    local_kmer_list->push_back( knfs );
                    while ( itr != last )
                    { unsigned char pred = kkk.front();
                        kkk.toSuccessor(*itr); ++itr;
                        kkk.kc = KMerContext(pred,*itr);
                        (kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk).to_struct(knfs);
                        local_kmer_list->push_back( knfs );
                    }
                    kkk.kc = KMerContext::finalContext(kkk.front());
                    kkk.toSuccessor(*last);
                    (kkk.isRev() ? KMerNodeFreq(kkk,true) : kkk).to_struct(knfs);
                    local_kmer_list->push_back( knfs );
                }
            }
            std::sort(local_kmer_list->begin(), local_kmer_list->end());
            collapse_entries(*local_kmer_list);
            //TODO: rather than "throwing the list for the master to merge", do the progressive merging
            //merge_level=0
            uint16_t merge_level=0;
            bool just_merged=true;
            //while (just merged)
            while (just_merged) {
                //   insert the list into this merge level's queue
                uint16_t slot=level_count[merge_level]++;

                //   if insertion number is odd or last batch:
                if (merge_level<levels-1 and (slot%2==1 or slot==batch_lists[merge_level].size()-1) ) {
                    //      if insertion number is odd:
                    if (slot%2==1) {
                        //          mix with the previous even number (using own list as base, this way we preserve locality)
                        while (batch_status[merge_level][slot-1]!=1) usleep(10); //wait for the previous batch to be finished.
//#pragma omp critical
//                        OutputLog(2)<<"merging level "<<merge_level<<" slots "<<slot-1<<" and "<< slot<<std::endl;
                        batch_status[merge_level][slot-1]=2;
                        batch_status[merge_level][slot]=2;
                        inplace_count_merge(local_kmer_list,batch_lists[merge_level][slot-1]);
                        batch_lists[merge_level][slot-1].reset();
                        //batch_lists[merge_level][slot].reset();
                    }
                    //      increase level
                    ++merge_level;
                    //      just_merged=true
                    just_merged=true;
                }
                //   else:
                else {
                    //      just_merged=false
//#pragma omp critical
//                    OutputLog(2)<<"thread iteration finished, results on "<<merge_level<<" slot "<<slot<<std::endl;
                    batch_lists[merge_level][slot]=local_kmer_list;
                    batch_status[merge_level][slot]=1;
                    just_merged=false;
                }
                //IDEA: fixed partitioning can be used to keep 2*thread-number lists for longer.
            }
            batch_lists[0][batch]=local_kmer_list;
//#pragma omp critical
//            OutputLog(4)<<"batch "<<batch <<" done"<<std::endl;
            batch_status[0][batch]=1;
        }

    }
    OutputLog(2)<<"Top level merge starting"<<std::endl;
    inplace_count_merge(batch_lists.back()[0],batch_lists.back()[1]);
    std::shared_ptr<std::vector<KMerNodeFreq_s>> kmer_list=batch_lists.back()[0];
    //batch_lists.back()[0].reset();
    //batch_lists.back()[1].reset();
    for (auto &bs:batch_status) free(bs);
    OutputLog(2)<<"Top level merge done"<<std::endl;
    //free(batch_status[0]);
    //free(batch_lists[0]);


    //sort
    //#pragma omp critical
    //std::cout << "createDictOMPRecursive (thread "<<omp_get_thread_num()<<") from " << from << " to " << to << ", sorting/collapsing "<<kmer_list.size()<<" kmers ..."
    //          << std::endl;


    if (NULL != dict) { //merge sort and return that
        OutputLog(2) << kmer_list->size() << " kmers counted, filtering..." << std::endl;
        (*dict) = new BRQ_Dict(kmer_list->size());
        uint64_t used = 0,not_used=0;
        uint64_t hist[256];
        for (auto &h:hist) h=0;
        for (auto &knfs:*kmer_list) {
            ++hist[std::min(255,(int)knfs.count)];
            if (knfs.count >= minFreq) {
                KMerNodeFreq knf(knfs);
                (*dict)->insertEntryNoLocking(BRQ_Entry((BRQ_Kmer)knf,knf.kc));
                used++;
            } else {
                not_used++;
            }

        }
        OutputLog(2) << used << " / " << kmer_list->size() << " kmers with Freq >= " << minFreq << std::endl;
        kmer_list->clear();
        if (""!=workdir) {
            std::ofstream kff(workdir + "/small_K.freqs");
            for (auto i = 1; i < 256; i++) kff << i << ", " << hist[i] << std::endl;
            kff.close();
        }

    }
    //return kmer_list;

}


void createDictOMPDiskBased(BRQ_Dict ** dict, vecbvec const& reads, std::vector<uint16_t> const& rlen, unsigned char disk_batches, uint64_t batch_size, unsigned minFreq, std::string workdir="", std::string tmpdir=""){
    //If size larger than batch (or still not enough cpus used, or whatever), Lauch 2 tasks to sort the 2 halves, with minFreq=0
    OutputLog(2) << "disk-based kmer counting with "<<(int) disk_batches<<" batches"<<std::endl;
    uint64_t total_kmers_in_batches=0;
    for (auto batch=0;batch < disk_batches;batch++) {
        uint64_t nkmers=0;
        //#pragma omp critical
        //std::cout << "createDictOMPRecursive (thread "<<omp_get_thread_num()<<") from " << from << " to " << to << ", splitting..." << std::endl;
        uint64_t to = (batch+1) * reads.size()/disk_batches;
        uint64_t from= batch * reads.size()/disk_batches;
        uint64_t mid_point = from + (to - from) / 2;
        std::vector<KMerNodeFreq> entries1, entries2; //TODO: need to not copy but move the reference.
#pragma omp task shared(reads,rlen,entries1)
        { entries1 = createDictOMPRecursive(NULL, reads, rlen, from, mid_point, minFreq, batch_size);}
#pragma omp task shared(reads,rlen,entries2)
        { entries2 = createDictOMPRecursive(NULL, reads, rlen, mid_point, to, minFreq, batch_size); }
#pragma omp taskwait
        auto end1=entries1.end(),end2=entries2.end();
        auto itr1=entries1.begin(),itr2=entries2.begin();
        std::ofstream batch_file(tmpdir+"/kmer_count_batch_"+std::to_string((int)batch),std::ios::out | std::ios::trunc | std::ios::binary);
        KMerNodeFreq knf;
        while (itr1<end1 and itr2<end2){
            if (*itr1==*itr2){
                knf=*itr1;
                combine_Entries(knf,*itr2);
                batch_file.write((const char *)&knf,sizeof(knf));
                ++itr1;
                ++itr2;
                ++nkmers;
            } else if (*itr1<*itr2){
                batch_file.write((const char *)&(*itr1),sizeof(knf));
                ++itr1;
                ++nkmers;
            } else {
                batch_file.write((const char *)&(*itr2),sizeof(knf));
                ++itr2;
                ++nkmers;
            }
        }
        while (itr1<end1) {
            batch_file.write((const char *)&(*itr1++),sizeof(knf));
            ++nkmers;
        }
        while (itr2<end2) {
            batch_file.write((const char *)&(*itr2++),sizeof(knf));
            ++nkmers;
        }
        entries1.clear();
        entries1.shrink_to_fit();
        entries2.clear();
        entries2.shrink_to_fit();
        batch_file.close();
        OutputLog(2) << "batch "<<(int) batch<<" done and dumped with "<<nkmers<< " kmers" <<std::endl;
        total_kmers_in_batches+=nkmers;
    }

    //now a multi-merge sort between all batch files into the Dict
    OutputLog(2) << "merging from disk"<<std::endl;
    //open all batch files
    std::ifstream dbf[disk_batches];
    bool dbf_active[disk_batches];
    KMerNodeFreq next_knf_from_dbf[disk_batches];
    uint finished_files=0;
    for (auto i=0;i<disk_batches;i++){
        dbf[i].open(tmpdir+"/kmer_count_batch_"+std::to_string((int)i),std::ios::in | std::ios::binary);
        dbf[i].read((char *)&next_knf_from_dbf[i],sizeof(KMerNodeFreq));
        dbf_active[i]=true;
    }
    //set all finished flags to false
    //TODO: stupid minimum search
    KMerNodeFreq current_kmer;
    //while finished_files<batches
    bool first=true;
    uint min=0;
    for (auto i=1;i<disk_batches;++i)
        if (dbf_active[i]){
            if (next_knf_from_dbf[i]<next_knf_from_dbf[min]) min=i;
        }
    current_kmer=next_knf_from_dbf[min];
    current_kmer.count=0;
    uint64_t used = 0,not_used=0;
    uint64_t hist[256];
    for (auto &h:hist) h=0;
    std::vector<KMerNodeFreq> kmerlist;

    while (finished_files<disk_batches) {
        //find minimum of the non-finished files
        uint min=0;
        for (auto i=1;i<disk_batches;++i)
            if (dbf_active[i]){
                if (next_knf_from_dbf[i]<next_knf_from_dbf[min]) min=i;
            }
        //larger than current kmer?
        if (next_knf_from_dbf[min] > current_kmer) {
            ++hist[std::min(255,(int)current_kmer.count)];
            if (current_kmer.count>=minFreq) {
                //(*dict)->insertEntryNoLocking(BRQ_Entry((BRQ_Kmer) current_kmer, current_kmer.kc));
                kmerlist.push_back(current_kmer);
                used++;
            }
            else not_used++;
            current_kmer=next_knf_from_dbf[min];
        } else {
            combine_Entries(current_kmer,next_knf_from_dbf[min]);
        }
        //advance min file
        dbf[min].read((char *)&next_knf_from_dbf[min],sizeof(KMerNodeFreq));
        if ( dbf[min].eof() ) {
            dbf_active[min]=false;
            ++finished_files;
        }
    }
    ++hist[std::min(100,(int)current_kmer.count)];
    if (current_kmer.count>=minFreq) {
        kmerlist.push_back(current_kmer);
        used++;
    }
    else not_used++;
    for (auto i=0;i<disk_batches;i++) {
        dbf[i].close();
        std::remove((tmpdir + "/kmer_count_batch_" +std::to_string((int)i)).c_str());
    }
    (*dict)=new BRQ_Dict(kmerlist.size());
    for (auto &knf: kmerlist) (*dict)->insertEntryNoLocking(BRQ_Entry((BRQ_Kmer) knf, knf.kc));
    OutputLog(2) << used << " / " << used+not_used << " kmers with Freq >= " << minFreq << std::endl;
    if (""!=workdir) {
        std::ofstream kff(workdir + "/small_K.freqs");
        for (auto i = 1; i < 256; i++) kff << i << ", " << hist[i] << std::endl;
        kff.close();
    }

}


void createDictOMPMemBased(BRQ_Dict ** dict, vecbvec const& reads, std::vector<uint16_t> const& rlen, unsigned char batches, uint64_t batch_size, unsigned minFreq, std::string workdir=""){
    //If size larger than batch (or still not enough cpus used, or whatever), Lauch 2 tasks to sort the 2 halves, with minFreq=0
    OutputLog(2) << "mem-based kmer counting with "<<(int) batches<<" batches"<<std::endl;

    uint64_t total_kmers_in_batches=0;

    std::vector<KMerNodeFreq> all_entries;
    for (auto batch=0;batch < batches;batch++) {
        //#pragma omp critical
        //std::cout << "createDictOMPRecursive (thread "<<omp_get_thread_num()<<") from " << from << " to " << to << ", splitting..." << std::endl;
        uint64_t to = (batch+1) * reads.size()/batches;
        uint64_t from= batch * reads.size()/batches;
        std::vector<KMerNodeFreq> new_entries; //TODO: need to not copy but move the reference.
        new_entries = createDictOMPRecursive(NULL, reads, rlen, from, to, batch_size, minFreq);
        OutputLog(2) << "batch "<<(int) batch<<" done, "<<new_entries.size()<< " kmers" <<std::endl;
        inplace_count_merge(all_entries,new_entries);
        OutputLog(2) << "batch "<<(int) batch<<" merged, "<<all_entries.size()<< " total kmers so far" <<std::endl;
    }

    OutputLog(2) << all_entries.size() << " kmers counted, filtering..." << std::endl;
    (*dict) = new BRQ_Dict(all_entries.size());
    uint64_t used = 0,not_used=0;
    uint64_t hist[256];
    for (auto &h:hist) h=0;
    for (auto &knf:all_entries) {
        ++hist[std::min(255,(int)knf.count)];
        if (knf.count >= minFreq) {
            (*dict)->insertEntryNoLocking(BRQ_Entry((BRQ_Kmer)knf,knf.kc));
            used++;
        } else {
            not_used++;
        }

    }
    OutputLog(2) << used << " / " << all_entries.size() << " kmers with Freq >= " << minFreq << std::endl;
    all_entries.clear();
    if (""!=workdir) {
        std::ofstream kff(workdir + "/small_K.freqs");
        for (auto i = 1; i < 256; i++) kff << i << ", " << hist[i] << std::endl;
        kff.close();
    }

}

void buildReadQGraph( vecbvec const& reads, VecPQVec &quals, std::vector<uint16_t> & rlen,
                      bool doFillGaps, bool doJoinOverlaps,
                      unsigned minFreq,
                      double minFreq2Fract, unsigned maxGapSize,
                      HyperBasevector* pHBV, ReadPathVec* pPaths,
                      int _K, std::string workdir, std::string tmpdir="",
                      unsigned char disk_batches, unsigned char mem_batches, uint64_t count_batch_size )
{
    OutputLog(2) << "creating kmers from reads..." << std::endl;
    //BRQ_Dict* pDict = createDictOMP(reads,quals,minQual,minFreq);
    BRQ_Dict * pDict;
    /*#pragma omp parallel shared(pDict,reads,rlen)
    {
        #pragma omp single
        {


            if (1 >= disk_batches) {
                if (mem_batches > 1)
                    createDictOMPMemBased(&pDict, reads, rlen, mem_batches, 1000000, minFreq, workdir);
                else*/
                    createDictOMP(&pDict, reads, rlen, count_batch_size, minFreq, workdir);
            /*} else {
                if ("" == tmpdir) tmpdir = workdir;
                createDictOMPDiskBased(&pDict, reads, rlen, disk_batches, 1000000, minFreq, workdir, tmpdir);

            }
        }
    }*/
    pDict->recomputeAdjacencies();
    OutputLog(2) << "finding edges (unique paths)" << std::endl;
    // figure out the complete base sequence of each edge
    vecbvec edges;
    edges.reserve(pDict->size()/100); //TODO: this is probably WAY too much in most scenarios
    buildEdges(*pDict,&edges);
    uint64_t totalk=0;
    for (auto &e:edges) totalk+=e.size()+1-K;
    OutputLog(2) <<edges.size()<<" edges with "<<totalk<<" "<<K<<"-mers"<<std::endl;

    unsigned minFreq2 = std::max(2u,unsigned(minFreq2Fract*minFreq+.5));

    if ( doFillGaps ) { // Off by default
        OutputLog(2) << "filling gaps." << std::endl;
        fillGaps(reads, maxGapSize, minFreq2, &edges, pDict);
    }

    if ( doJoinOverlaps ) { // Off by default
        OutputLog(2) << "joining Overlaps." << std::endl;
        joinOverlaps(reads, _K / 2, minFreq2, &edges, pDict);
    }

    std::vector<int> fwdEdgeXlat;
    std::vector<int> revEdgeXlat;
    if ( !pPaths )
    {
        delete pDict;
        OutputLog(2) << "building graph..." << std::endl;
        buildHBVFromEdges(edges,_K,pHBV,fwdEdgeXlat,revEdgeXlat);
        OutputLog(2) << "graph built" << std::endl;

    }
    else
    {
        OutputLog(2) << "building graph..." << std::endl;
        buildHBVFromEdges(edges,K,pHBV,fwdEdgeXlat,revEdgeXlat);
        OutputLog(2) << "Loading quals..." << std::endl;
        quals.ReadAll(workdir + "/frag_reads_orig.qualp");
        OutputLog(2) << "pathing reads into graph..." << std::endl;
        pPaths->clear();
        pPaths->resize(reads.size());


        path_reads_OMP(reads, quals, *pDict, edges, *pHBV, fwdEdgeXlat, revEdgeXlat, pPaths);
        delete pDict;
        OutputLog(2) << "reads pathed"<<std::endl;
    }

}