 /*
 * Copyright 2018, Chanhee Park <parkchanhee@gmail.com> and Daehwan Kim <infphilo@gmail.com>
 *
 * This file is part of HISAT 2.
 *
 * HISAT 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HISAT 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HISAT 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __REPEAT_KMER_H__
#define __REPEAT_KMER_H__

#include <iostream>
#include <fstream>
#include <limits>
#include <map>
#include "assert_helpers.h"
#include "word_io.h"
#include "mem_ids.h"
#include "ds.h"

class RB_Minimizer {
public:
    template<typename TStr>
    static pair<uint64_t, size_t>
    get_minimizer(const TStr& seq,
                  size_t off,
                  size_t window,
                  size_t k)
    {
        assert_leq(k, 32);
        assert_leq(off + window + k - 1, seq.length());
        pair<uint64_t, size_t> minimizer(get_kmer(seq, off, k), off);
        uint64_t kmer = minimizer.first;
        for(size_t i = off + 1; i < off + window; i++) {
            uint64_t next_kmer = get_next_kmer(kmer, seq[i+k-1], k);
            if(minimizer_leq(next_kmer, minimizer.first)) {
                minimizer.first = next_kmer;
                minimizer.second = i;
            }
            kmer = next_kmer;
        }
        return minimizer;
    }
    
    template<typename TStr>
    static void
    get_minimizer(const TStr& seq,
                  size_t window,
                  size_t k,
                  EList<pair<uint64_t, size_t> >& minimizers)
    {
        assert_leq(k, 32);
        assert_leq(window + k - 1, seq.length());
        
        minimizers.clear();
        pair<uint64_t, size_t> minimizer = get_minimizer(seq, 0, window, k);
        minimizers.push_back(minimizer);
        uint64_t kmer = get_kmer(seq, window - 1, k);
        for(size_t i = 1; i + window + k - 1 <= seq.length(); i++) {
            uint64_t next_kmer = get_next_kmer(kmer, seq[i+window+k-2], k);
            if(minimizer.second < i) {
               minimizer = get_minimizer(seq, i, window, k);
            } else if(minimizer_leq(next_kmer, minimizer.first)) {
                minimizer.first = next_kmer;
                minimizer.second = i + window - 1;
            }
            minimizers.push_back(minimizer);
            kmer = next_kmer;
        }
        
#ifndef NDEBUG
        assert_eq(minimizers.size() + window + k - 2, seq.length());
        for(size_t i = 0; i + window + k - 1 <= seq.length(); i++) {
            pair<uint64_t, size_t> minimizer = get_minimizer(seq, i, window, k);
            assert(minimizer == minimizers[i]);
        }
#endif
    }
    
protected:
    static bool
    minimizer_leq(uint64_t kmer, uint64_t kmer2)
    {
#if 0
        kmer = convert_minimizer(kmer);
        kmer2 = convert_minimizer(kmer2);
#endif
        return kmer <= kmer2;
    }
    
    // Heng Li's Minimap and minaisam paper, 2016
    static uint64_t convert_minimizer(uint64_t x) {
        x = (~x) + (x << 21);
        x = x ^ (x >> 24);
        x = x + (x << 3) + (x << 8);
        x = x ^ (x >> 14);
        x = x + (x << 2) + (x << 4);
        x = x ^ (x >> 28);
        x = x + (x << 31);
        return x;
    }
    
    template<typename TStr>
    static uint64_t
    get_kmer(const TStr& seq,
             size_t offset,
             size_t k)
    {
        assert_leq(offset + k, seq.length());
        uint64_t kmer = 0;
        for(size_t i = 0; i < k; i++) {
            size_t c = seq[offset + i];
            if(c > 3) c = asc2dna[c];
            kmer = (kmer << 2 | c);
        }
        return kmer;
    }
    
    static uint64_t
    get_next_kmer(uint64_t kmer,
                  size_t base,
                  size_t k)
    {
        kmer &= (((uint64_t)1 << ((k-1)*2))) - 1;
        if(base > 3) base = asc2dna[base];
        kmer = (kmer << 2) | base;
        return kmer;
    }
    
    template<typename TStr>
    static TStr get_string(uint64_t kmer, size_t k)
    {
        TStr seq = "";
        for(size_t i = 0; i < k; i++) {
            size_t nt = kmer & 0x3;
            seq.push_back("ACGT"[nt]);
            kmer >>= 2;
        }
        reverse(seq.begin(), seq.end());
        return seq;
    }
};

class RB_KmerTable {
public:
    RB_KmerTable() { w_ = k_ = 0; }
    ~RB_KmerTable() {}
    
public:
    bool isIn(uint64_t kmer) const
    {
        pair<uint64_t, TIndexOffU> key(kmer, 0);
        size_t idx = kmer_table_.bsearchLoBound(key);
        return idx < kmer_table_.size() && kmer_table_[idx].first == kmer;
    }
    
    template<typename TStr>
    bool isRepeat(const TStr& query,
                  const TStr& rc_query,
                  EList<pair<uint64_t, size_t> >& minimizers) const
    {
        return isRepeat(query, minimizers) || isRepeat(rc_query, minimizers);
    }
    
    template<typename TStr>
    bool isRepeat(const TStr& query,
                  EList<pair<uint64_t, size_t> >& minimizers) const
    {
        RB_Minimizer::get_minimizer(query, w_, k_, minimizers);
        size_t est_count = 0;
        uint64_t prev_minimizer = 0;
        bool prev_in = false;
        for(size_t j = 0; j < minimizers.size(); j++) {
            bool curr_in = false;
            if(minimizers[j].first == prev_minimizer) {
                if(prev_in) est_count++;
                curr_in = prev_in;
            } else if(isIn(minimizers[j].first)) {
                curr_in = true;
                est_count++;
                
                return true;
            }
            prev_minimizer = minimizers[j].first;
            prev_in = curr_in;
        }
        
#if 1
        return false;
#else
        bool est_repeat = est_count * 2 >= minimizers.size();
        return est_repeat;
#endif
    }
    
    template<typename TStr>
    void findRepeats(const TStr& query,
                     EList<pair<uint64_t, size_t> >& minimizers,
                     EList<TIndexOffU>& repeats) const
    {
        repeats.clear();
        RB_Minimizer::get_minimizer(query, w_, k_, minimizers);
        for(size_t i = 0; i < minimizers.size(); i++) {
            if(i > 0 && minimizers[i].first == minimizers[i-1].first)
                continue;
            pair<uint64_t, size_t> minimizer(minimizers[i].first, 0);
            size_t j = kmer_table_.bsearchLoBound(minimizer);
            for(; j < kmer_table_.size() && minimizer.first == kmer_table_[j].first; j++) {
                repeats.push_back(kmer_table_[j].second);
            }
        }
        if(repeats.empty())
            return;
        
        size_t remove_count = 0;
        repeats.sort();
        for(size_t i = 0; i + 1 < repeats.size();) {
            size_t j = i + 1;
            for(; j < repeats.size(); j++) {
                if(repeats[i] == repeats[j]) {
                    repeats[j] = std::numeric_limits<TIndexOffU>::max();
                    remove_count++;
                } else break;
            }
            i = j;
        }
        repeats.sort();
        assert_lt(remove_count, repeats.size());
        repeats.resize(repeats.size() - remove_count);
    }
    
    bool write(ofstream& f_out, bool bigEndian) const {
        writeIndex<size_t>(f_out, w_, bigEndian);
        writeIndex<size_t>(f_out, k_, bigEndian);
        writeIndex<size_t>(f_out, kmer_table_.size(), bigEndian);
        for(size_t i = 0; i < kmer_table_.size(); i++) {
            writeIndex<size_t>(f_out, kmer_table_[i].first, bigEndian);
            if(sizeof(TIndexOffU) == 4) {
                writeU32(f_out, kmer_table_[i].second, bigEndian);
            } else {
                assert_eq(sizeof(TIndexOffU), 8);
                writeIndex<uint64_t>(f_out, kmer_table_[i].second, bigEndian);
            }
        }
        writeIndex<size_t>(f_out, pos_list_.size(), bigEndian);
        for(size_t i = 0; i < pos_list_.size(); i++) {
            if(sizeof(TIndexOffU) == 4) {
                writeU32(f_out, pos_list_[i], bigEndian);
            } else {
                assert_eq(sizeof(TIndexOffU), 8);
                writeIndex<uint64_t>(f_out, pos_list_[i], bigEndian);
            }
        }
        return true;
    }
    
    bool read(ifstream& f_in, bool bigEndian) {
        w_ = readIndex<size_t>(f_in, bigEndian);
        k_ = readIndex<size_t>(f_in, bigEndian);
        size_t kmer_size = readIndex<size_t>(f_in, bigEndian);
        kmer_table_.reserveExact(kmer_size);
        while(kmer_table_.size() < kmer_size) {
            kmer_table_.expand();
            kmer_table_.back().first = readIndex<size_t>(f_in, bigEndian);
            if(sizeof(TIndexOffU) == 4) {
                kmer_table_.back().second = readU32(f_in, bigEndian);
            } else {
                assert_eq(sizeof(TIndexOffU), 8);
                kmer_table_.back().second = readIndex<uint64_t>(f_in, bigEndian);
            }
        }
        size_t pos_size = readIndex<size_t>(f_in, bigEndian);
        pos_list_.reserveExact(pos_size);
        while(pos_list_.size() < pos_size) {
            pos_list_.expand();
            if(sizeof(TIndexOffU) == 4) {
                pos_list_.back() = readU32(f_in, bigEndian);
            } else {
                assert_eq(sizeof(TIndexOffU), 8);
                pos_list_.back() = readIndex<uint64_t>(f_in, bigEndian);
            }
        }
        return true;
    }

public:
    template<typename TStr>
    void build(const EList<TStr>& seqs,
               size_t w,
               size_t k)
    {
        w_ = w;
        k_ = k;
        kmer_table_.clear();
        pos_list_.clear();

        EList<pair<uint64_t, TIndexOffU> > tmp_table;
        set<uint64_t> tmp_kmers;
        
        TIndexOffU baseoff = 0;
        EList<pair<uint64_t, size_t> > minimizers;
        for(size_t s = 0; s < seqs.size(); s++) {
            const TStr& seq = seqs[s];
            RB_Minimizer::get_minimizer(seq,
                                        w_,
                                        k_,
                                        minimizers);
            for(size_t i = 0; i < minimizers.size(); i++) {
                if(!tmp_table.empty() &&
                   tmp_table.back().first == minimizers[i].first &&
                   tmp_table.back().second == baseoff + minimizers[i].second)
                    continue;
                tmp_table.expand();
                tmp_table.back().first = minimizers[i].first;
                tmp_table.back().second = baseoff + minimizers[i].second;
                tmp_kmers.insert(minimizers[i].first);
            }
            baseoff += seq.length();
        }
        tmp_table.sort();

        kmer_table_.reserveExact(tmp_kmers.size());
        pos_list_.reserveExact(tmp_table.size());
        for(size_t i = 0; i < tmp_table.size(); i++) {
#ifndef NDEBUG
            if(!pos_list_.empty()) {
                assert_neq(pos_list_.back(), tmp_table[i].second);
            }
#endif
            if(kmer_table_.empty() || kmer_table_.back().first != tmp_table[i].first) {
                kmer_table_.expand();
                kmer_table_.back().first = tmp_table[i].first;
                kmer_table_.back().second = pos_list_.size();
            }
            pos_list_.push_back(tmp_table[i].second);
        }
        assert_eq(kmer_table_.size(), tmp_kmers.size());
        assert_eq(pos_list_.size(), tmp_table.size());
    }
    
    void dump(ostream& o) const
    {
        o << "window         : " << w_ << endl;
        o << "k length       : " << k_ << endl;
        o << "number of kmer : " << kmer_table_.size() << endl;
        o << "number of pos  : " << pos_list_.size() << endl;
    }

private:
    size_t w_;
    size_t k_;
    EList<pair<uint64_t, TIndexOffU> > kmer_table_;
    EList<TIndexOffU> pos_list_;
};


#endif /* __REPEAT_KMER_H__ */
