//
// Created by Luis Yanes (EI) on 2019-04-29.
//

#include "SpectraCn.hpp"

/**
 * Only required when the input kmers are in unsorted order
 * @param file Kmer count data file
 * @param target Kmer this function will be looking for in the kmer data file
 * @param lo start of the search range
 * @param hi end of the search range
 * @return frequency of the target kmer in the frequencies file
 */
uint8_t SpectraCN::findCountBin(std::ifstream &file, KMerNodeFreq_s &target, size_t lo, size_t hi) {
    while (lo <= hi) {
        size_t mid = (size_t) (lo + (hi - lo) / 2);
        std::streamoff offset;
        offset = sizeof(uint64_t) + mid * sizeof(KMerNodeFreq_s);
        file.seekg(offset, std::ios_base::beg);
        KMerNodeFreq_s sKmer;
        file.read((char *) &sKmer, sizeof(KMerNodeFreq_s));

        if (sKmer == target) {
            return sKmer.count;
        } else if (sKmer < target) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
//    std::cerr << "Kmer not found" << std::endl;
    return 0;
}

/**
 * @brief This function receives a hbv + involution graph and prints out a sparse spectra-cn file
 * @param hb Graph
 * @param inv Graph Involution
 * @param dir Output directory, this has to be the same as the rest of the assembly
 * @param name Name of the spectra-cn file (without extension)
 */
void SpectraCN::DumpSpectraCN(const HyperBasevector &hb, const vec<int> &inv, const String &dir, const String &name) {

    std::shared_ptr<KmerList> local_kmer_list = std::make_shared<KmerList>();
    uint64_t total_good_length(0);
    // Calculate good length from the "lines"
    for (uint64_t edgeID = 0; edgeID < hb.E(); edgeID++) {
        if (inv[edgeID] < edgeID) continue;
        total_good_length += hb.EdgeLengthBases(edgeID);
    }
    local_kmer_list->resize(total_good_length);

    //Populate the kmer list
    uint64_t last_kmer=0;
    for (auto edgeID = 0; edgeID  < hb.E(); ++edgeID) {
        if (inv[edgeID] < edgeID) continue;
        unsigned len = hb.EdgeLengthBases(edgeID);
        if (len > K) {
            auto beg = hb.EdgeObject(edgeID).begin(), itr = beg + K, last = beg + (len - 1);
            KMerNodeFreq kkk(beg);
            kkk.hash();
            kkk.kc = KMerContext::initialContext(*itr);
            kkk.count = 1;
            (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
            ++last_kmer;
            while (itr != last) {
                unsigned char pred = kkk.front();
                kkk.toSuccessor(*itr);
                ++itr;
                kkk.kc = KMerContext(pred, *itr);
                (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
                ++last_kmer;
            }
            kkk.kc = KMerContext::finalContext(kkk.front());
            kkk.toSuccessor(*last);
            (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
            ++last_kmer;
        }
    }
    //std::cout<<last_kmer<<" kmers inserted in a batch"<<std::endl;
    local_kmer_list->resize(last_kmer);
    local_kmer_list->sort();
    local_kmer_list->uniq();


    // This is the total list of kmers in the graph, now intersect with kmers from reads and generate the output
    uint64_t numKmers(0);
    std::ifstream kmerData;
    kmerData.open(dir + "/raw_kmers.data");
    kmerData.read((char *) &numKmers, sizeof(uint64_t));

    std::FILE* kmers_from_disk;
    kmers_from_disk = std::fopen(std::string(dir+"/raw_kmers.data").data(), "rb");
    if (!kmers_from_disk) {
        std::perror("Failed to open raw_kmers.data, ");
    }
    auto read = std::fread(&numKmers, sizeof(uint64_t), 1, kmers_from_disk);
    if (!read) {
        std::perror("Failed to read kmer from disk,");
    }

    std::map< std::vector<uint64_t>, uint64_t > totals_by_freq;
    KMerNodeFreq_s target;
    read = std::fread(&target, sizeof(target), 1, kmers_from_disk);
    if (!read) {
        std::perror("Failed to read kmer from disk, ");
    }
    uint64_t read_disk_kmers(1);
    for (uint64_t query = 0; query < local_kmer_list->size; ++query) {
        const auto &qkmer=local_kmer_list->kmers[query];
        if (qkmer > target) {
            do {
                read = std::fread(&target, sizeof(target), 1, kmers_from_disk);
                ++read_disk_kmers;
                if (!read) {
                    std::perror("Failed to read kmer from disk, ");
                }
            } while (qkmer > target);
        } else if (qkmer < target) {
            ++totals_by_freq[{qkmer.count, 0}];
        } else {
            ++totals_by_freq[{qkmer.count, target.count}];
        }
        if (qkmer == target) {
            ++totals_by_freq[{qkmer.count, target.count}];
        } else {
            ++totals_by_freq[{qkmer.count, 0}];
        }

        if (read_disk_kmers == numKmers) {
            // There are no more kmers to be found in disk, dump last asm kmers missing from disk and break
            while (query < local_kmer_list->size) {
                const auto &qkmer=local_kmer_list->kmers[query];
                ++totals_by_freq[{qkmer.count, 0}];
                ++query;
            }
            break;
        }
    }

    std::ofstream analysis_output(dir+"/"+name+".freqs");
    for (int i = 1; i <= 2; i++) {
        analysis_output << "f" + std::to_string(i-1) + ",";
    }
    analysis_output<<"kmers"<<std::endl;
    std::cout <<"kmers"<<std::endl;
    for(const auto &fc:totals_by_freq) {
        for (const auto &s : fc.first) {
            analysis_output << s << ",";
        }
        analysis_output << fc.second << std::endl;
    }
}

/**
 * @brief This function receives a path to a fasta file, and the assembly directory and writes a sparse spectra-cn file
 * @param fasta_path Path to the fasta file used as the assembly
 * @param dir Path to the output dir of an assembly (raw_kmers.data has to be present there!)
 * @param name Name of the output file
 */
void SpectraCN::DumpSpectraCN(const std::string &fasta_path, const std::string &dir, const std::string &name) {

    auto total_good_length = get_total_fasta_length(fasta_path);

    gzFile fp1 = gzopen(fasta_path.c_str(), "r");
    FunctorZlib gzr1;
    kstream<gzFile, FunctorZlib> ks1(fp1, gzr1);
    int finished_early;

    std::shared_ptr<KmerList> local_kmer_list = std::make_shared<KmerList>();
    local_kmer_list->resize(total_good_length);
    fp1 = gzopen(fasta_path.c_str(), "r");

    kseq seq1;
    int l1=0;
    int c1=0;
    l1 = ks1.read(seq1);
    uint64_t last_kmer=0;
    while ( l1 >= 0 ) {
        if (seq1.seq.empty() ) {
            std::cout << "Error " << std::string(fasta_path.c_str()) << " on read " << c1 << " is invalid" << std::endl;
            finished_early=1;
            break;
        }
        CharToBaseMapper c2b;
        // Process this sequence!
        unsigned len = seq1.seq.size();
        if (len > K) {
            auto beg = seq1.seq.begin(), itr = beg + K, last = beg + (len - 1);
            KMerNodeFreq kkk(beg, true);
            kkk.hash();
            kkk.count = 1;
            (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
            ++last_kmer;
            while (itr != last) {
                unsigned char pred = kkk.front();
                kkk.toSuccessor(c2b(*itr));
                ++itr;
                (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
                ++last_kmer;
            }
            kkk.toSuccessor(c2b(*last));
            (kkk.isRev() ? KMerNodeFreq(kkk, true) : kkk).to_struct( local_kmer_list->kmers[last_kmer] );
            ++last_kmer;
        }

        c1++;
        l1 = ks1.read(seq1);
    }

    if (!finished_early) {
        if (l1 == -2) {
            std::cout << std::string(fasta_path.c_str()) << " is invalid at " << c1 << std::endl;
        }
    }
    gzclose(fp1);


    local_kmer_list->resize(last_kmer);
    local_kmer_list->sort();
    local_kmer_list->uniq();


    // This is the total list of kmers in the graph, now intersect with kmers from reads and generate the output
    uint64_t numKmers(0);
    std::FILE* kmers_from_disk;
    kmers_from_disk = std::fopen(std::string(dir+"/raw_kmers.data").data(), "rb");
    if (!kmers_from_disk) {
        std::perror("Failed to open raw_kmers.data, ");
    }
    auto read = std::fread(&numKmers, sizeof(uint64_t), 1, kmers_from_disk);
    if (!read) {
        std::perror("Failed to read kmer from disk,");
    }

    std::map< std::vector<uint64_t>, uint64_t > totals_by_freq;
    KMerNodeFreq_s target;
    read = std::fread(&target, sizeof(target), 1, kmers_from_disk);
    if (!read) {
        std::perror("Failed to read kmer from disk, ");
    }
    uint64_t read_disk_kmers(1);
    for (uint64_t query = 0; query < local_kmer_list->size; ++query) {
        const auto &qkmer=local_kmer_list->kmers[query];
        if (qkmer > target) {
            do {
                read = std::fread(&target, sizeof(target), 1, kmers_from_disk);
                ++read_disk_kmers;
                if (!read) {
                    std::perror("Failed to read kmer from disk, ");
                }
            } while (qkmer > target);
        } else if (qkmer < target) {
            ++totals_by_freq[{qkmer.count, 0}];
        } else {
            ++totals_by_freq[{qkmer.count, target.count}];
        }
        if (qkmer == target) {
            ++totals_by_freq[{qkmer.count, target.count}];
        } else {
            ++totals_by_freq[{qkmer.count, 0}];
        }

        if (read_disk_kmers == numKmers) {
            // There are no more kmers to be found in disk, dump last asm kmers missing from disk and break
            while (query < local_kmer_list->size) {
                const auto &qkmer=local_kmer_list->kmers[query];
                ++totals_by_freq[{qkmer.count, 0}];
                ++query;
            }
            break;
        }
    }

    std::ofstream analysis_output(dir+"/"+name+".freqs");
    for (int i = 1; i <= 2; i++) {
        analysis_output << "f" + std::to_string(i-1) + ",";
    }
    analysis_output<<"kmers"<<std::endl;
    std::cout <<"kmers"<<std::endl;
    for(const auto &fc:totals_by_freq) {
        for (const auto &s : fc.first) {
            analysis_output << s << ",";
        }
        analysis_output << fc.second << std::endl;
    }

}