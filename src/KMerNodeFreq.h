//
// Created by Luis Yanes (EI) on 15/05/2017.
//

#ifndef W2RAP_CONTIGGER_KMerNodeFreq_H
#define W2RAP_CONTIGGER_KMerNodeFreq_H

#include "kmers/KMerContext.h"
#include "kmers/KMer.h"

const unsigned K = 60;


typedef struct __attribute__((__packed__)) KMerNodeFreq_s {
    uint64_t kdata[2];
    uint8_t count;
    uint8_t kc;

    inline const operator==(KMerNodeFreq_s const &other) const {
        //return 0==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
        return (kdata[0] == other.kdata[0] and kdata[1] == other.kdata[1]);
    }

    inline const operator<(KMerNodeFreq_s const &other) const {
        //return -1==memcmp(&kdata,&other.kdata,2*sizeof(uint64_t));
        if (kdata[0] < other.kdata[0]) return true;
        if (kdata[0] > other.kdata[0]) return false;
        return kdata[1] < other.kdata[1];
    }

    inline const operator>(KMerNodeFreq_s const &other) const {
        if (kdata[0] > other.kdata[0]) return true;
        if (kdata[0] < other.kdata[0]) return false;
        return kdata[1] > other.kdata[1];
    }

    inline void combine(KMerNodeFreq_s const &other) {
        auto newcount = count + other.count;
        if (newcount > count) count = newcount;
        kc |= other.kc;
    }
    /*inline const operator=(KMerNodeFreq_s const & other) const {
        memcpy(&this,&other,sizeof(this));
    }*/
};

class KMerNodeFreq : public KMer<K> {
public:
    KMerNodeFreq() {};

    template<class Itr>
    explicit KMerNodeFreq(Itr start) { assign(start, NopMapper()); }

    KMerNodeFreq(const KMerNodeFreq &other) {
        *this = other;
        count = other.count;
        kc = other.kc;
    }

    KMerNodeFreq(const KMerNodeFreq_s &other) {
        memcpy(&this->mVal, &other.kdata, sizeof(KMer < K > ));
        count = other.count;
        kc.mVal = other.kc;
    }

    KMerNodeFreq(const KMerNodeFreq &other, bool rc) {
        *this = other;
        count = other.count;
        kc = other.kc;
        if (rc) {
            this->rc();
            kc = kc.rc();
        }
    }

    void to_struct(KMerNodeFreq_s &other) const {
        memcpy(&other.kdata, &this->mVal, sizeof(KMer < K > ));
        other.count = count;
        other.kc = kc.mVal;
    }

    unsigned char count;

    KMerContext kc;
};

#endif //W2RAP_CONTIGGER_KMerNodeFreq_H
