/* Copyright (C) 2018-2020 Andrew D. Smith
 *
 * Authors: Andrew D. Smith
 *
 * This file is part of ABISMAL.
 *
 * ABISMAL is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ABISMAL is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>

#include "smithlab_os.hpp"
#include "smithlab_utils.hpp"
#include "OptionParser.hpp"
#include "zlib_wrapper.hpp"
#include "sam_record.hpp"
#include "bisulfite_utils.hpp"

#include "dna_four_bit_bisulfite.hpp"
#include "AbismalIndex.hpp"
#include "AbismalAlign.hpp"

#include <omp.h>

using std::vector;
using std::runtime_error;
using std::string;
using std::cerr;
using std::endl;
using std::cout;
using std::transform;
using std::numeric_limits;
using std::ostream;
using std::ofstream;
using std::max;
using std::min;
using std::to_string;
using std::begin;
using std::end;

typedef uint16_t flags_t; // every bit is a flag
typedef int16_t score_t; // aln score, edit distance, hamming distance
typedef vector<uint8_t> Read; //4-bit encoding of reads

enum conversion_type { t_rich = false, a_rich = true };

constexpr conversion_type
flip_conv(const conversion_type conv) {
  return conv == t_rich ? a_rich : t_rich;
}

constexpr flags_t
get_strand_code(const char strand, const conversion_type conv) {
  return (((strand == '-')  ? samflags::read_rc : 0) |
          ((conv == a_rich) ? bsflags::read_is_a_rich: 0));
}

struct ReadLoader {
  ReadLoader(const string &fn,
             const size_t bs = numeric_limits<size_t>::max()) :
    filename(fn), batch_size(bs) {
    in = new igzfstream(fn);
    if (!in || !(*in))
      throw runtime_error("bad reads file: " + filename);
  }
  ~ReadLoader() {delete in;}
  bool good() const {return bool(*in);}
  size_t get_current_byte() const {return gzoffset(in->fileobj);}
  void load_reads(vector<string> &names, vector<string> &reads) {
    static const size_t reserve_size = 250;

    reads.clear();
    names.clear();

    size_t line_count = 0;
    const size_t num_lines_to_read = 4*batch_size;
    string line;
    line.reserve(reserve_size);
    while (line_count < num_lines_to_read && bool(getline(*in, line))) {
      if (line_count % 4 == 0) {
        names.push_back(line.substr(1, line.find_first_of(" \t") - 1));
      }
      else if (line_count % 4 == 1) {
        if (count_if(begin(line), end(line),
                     [](const char c) {return c != 'N';}) < min_read_length)
          line.clear();
        reads.push_back(line);
      }
      ++line_count;
    }
    in->peek(); // needed in case batch_size exactly divides the
                // number of reads in the file
  }

  string filename;
  size_t batch_size;
  igzfstream *in;

  static uint32_t min_read_length;
};

uint32_t ReadLoader::min_read_length = seed::n_seed_positions + 1;

inline void
update_max_read_length(size_t &max_length, const vector<string> &reads) {
  for (auto it (begin(reads)); it != end(reads); ++it)
    max_length = std::max(max_length, it->size());
}

struct se_element { //assert(sizeof(se_element) == 8)
  uint32_t pos;
  score_t diffs;
  flags_t flags;

  se_element() :
    pos(0), diffs(0),  flags(0) {}

  se_element(const uint32_t p, const score_t d,
             const flags_t f) :
    pos(p), diffs(d), flags(f) {}

  bool operator==(const se_element &rhs) const {
    return diffs == rhs.diffs && pos == rhs.pos;
  }

  // this is used to keep PE candidates sorted in the max heap
  bool operator<(const se_element &rhs) const {
    return diffs < rhs.diffs;
  }
  inline bool is_better_than (const se_element &rhs) const {
    return diffs < rhs.diffs;
  }
  inline bool rc() const {return samflags::check(flags, samflags::read_rc);}
  inline bool elem_is_a_rich() const {
    return samflags::check(flags, bsflags::read_is_a_rich);
  }
  inline score_t invalid_hit_threshold(const uint32_t readlen) const {
    return static_cast<score_t>(readlen * invalid_hit_frac);
  }
  bool valid_hit(const uint32_t readlen) const {
    return diffs < invalid_hit_threshold(readlen);
  }
  bool valid(const size_t readlen) const {
    return valid_hit(readlen) &&
           diffs <= static_cast<score_t>(valid_frac * readlen);
  }
  void reset(const uint32_t readlen) {
    diffs = invalid_hit_threshold(readlen);
    pos = 0;
  }
  bool is_equal_to(const se_element &rhs) const {
    return (diffs == rhs.diffs) && (pos != rhs.pos);
  }
  static uint16_t min_aligned_length;
  static double valid_frac;
  static double invalid_hit_frac;
};

uint16_t se_element::min_aligned_length = ReadLoader::min_read_length;
double se_element::valid_frac = 0.1;
double se_element::invalid_hit_frac = 0.4;

struct se_result { //assert(sizeof(se_result) == 16)
  se_element best;
  se_element second_best;
  se_result() : best(se_element()), second_best(se_element()) {}

  void update(const uint32_t p, const score_t d, const flags_t s) {
    // avoid having two copies of the best hit
    if (p == best.pos && s == best.flags) return;

    const se_element cand(p, d, s);
    if (cand.is_better_than(second_best)) {
      second_best = cand;
      if (second_best.is_better_than(best)) std::swap(best, second_best);
    }
  }
  bool sort_candidates() {
    if (second_best.is_better_than(best)) {
      std::swap(best, second_best);
      return true;
    }
    return false;
  }
  bool valid(const size_t readlen) const { return best.valid(readlen); }
  bool valid_hit(const uint32_t readlen) const {
    return best.valid_hit(readlen);
  }
  bool ambig() const { return best.is_equal_to(second_best); }
  bool sure_ambig(const score_t seed_number = 0) const {
    return ambig() && (second_best.diffs <= seed_number);
  }
  bool optimal() const {
    return best.diffs == 0;
  }
  void reset(const uint32_t readlen)  {
    best.reset(readlen);
    second_best.reset(readlen);
  }
  uint32_t get_cutoff() const { return second_best.diffs + 1; }
};

inline bool
chrom_and_posn(const ChromLookup &cl, const string &cig, const uint32_t p,
               uint32_t &r_p, uint32_t &r_e, uint32_t &r_chr) {
  const uint32_t ref_ops = cigar_rseq_ops(cig);
  if (!cl.get_chrom_idx_and_offset(p, ref_ops, r_chr, r_p)) return false;
  r_e = r_p + ref_ops;
  return true;
}

enum map_type { map_unmapped, map_unique, map_ambig };
static map_type
format_se(const bool allow_ambig, se_result res, const ChromLookup &cl,
          const string &read, const string &read_name, const string &cigar,
          ostream &out) {
  const bool ambig = res.ambig();
  if (!allow_ambig && ambig)
    return map_ambig;

  const se_element s = res.best;

  uint32_t ref_s = 0, ref_e = 0, chrom_idx = 0;
  if (!s.valid(read.size()) ||
      !chrom_and_posn(cl, cigar, s.pos, ref_s, ref_e, chrom_idx))
    return map_unmapped;

  sam_rec sr(read_name, 0, cl.names[chrom_idx], ref_s + 1,
             255, cigar, "*", 0, 0, read, "*");
  if (s.rc())
    set_flag(sr, samflags::read_rc);

  if (allow_ambig && res.ambig())
    set_flag(sr, samflags::secondary_aln);

  sr.add_tag("NM:i:" + to_string(s.diffs));
  sr.add_tag(s.elem_is_a_rich() ? "CV:A:A" : "CV:A:T");

  out << sr << "\n";
  return ambig ? map_ambig : map_unique;
}

struct pe_element { //assert(sizeof(pe_element) == 16)
  se_element r1;
  se_element r2;

  pe_element() : r1(se_element()), r2(se_element()) {}
  pe_element(const se_element &s1, const se_element &s2) : r1(s1), r2(s2) {}

  score_t diffs() const { return r1.diffs + r2.diffs; }

  inline bool valid(const size_t readlen1, const size_t readlen2) const {
    return r1.diffs + r2.diffs <= static_cast<score_t>(
             se_element::valid_frac*(readlen1 + readlen2)
           );
  }
  inline bool is_equal_to(const pe_element &rhs) const {
    return diffs() == rhs.diffs() &&
           !(r1.pos == rhs.r1.pos && r2.pos == rhs.r2.pos);
  }
  inline bool is_better_than(const pe_element &rhs) const {
    return diffs() < rhs.diffs();
  }
  inline void reset(const uint32_t readlen1, const uint32_t readlen2) {
    r1.reset(readlen1);
    r2.reset(readlen2);
  }
  static uint32_t min_dist;
  static uint32_t max_dist;
};
uint32_t pe_element::min_dist = 32;
uint32_t pe_element::max_dist = 3000;

struct pe_result { // assert(sizeof(pe_result) == 16);
  pe_element best;
  pe_element second_best;
  pe_result() {}
  pe_result(const pe_element &a, const pe_element &b)
    : best(a), second_best(b) {}

  // true if best was updated. We use the value returned by this
  // function to update the reported cigar if necessary
  bool update(const pe_element &cand) {
    if (cand.is_better_than(second_best)) {
      second_best = cand;
      if (second_best.is_better_than(best)) {
        std::swap(second_best, best);
        return true;
      }
    }
    return false;
  }

  void reset(const uint32_t readlen1, const uint32_t readlen2) {
    best.reset(readlen1, readlen2);
    second_best.reset(readlen1, readlen2);
  }
  bool ambig() const {
    return best.is_equal_to(second_best);
  }
  bool valid(const size_t readlen1, const size_t readlen2) const {
    return best.valid(readlen1, readlen2);
  }
};

/* The results passed into format_pe should be on opposite strands
 * already, as those are the only valid pairings. They also should
 * have opposite "richness" for the same reason.
 *
 * On output, each read sequence is exactly as it appears in the input
 * FASTQ files. The strand is opposite for each end, and the richness
 * as well. Positions are incremented, since the SAM format is
 * 1-based. CIGAR strings are written just as they were constructed:
 * always starting with the first position on the reference,
 * regardless of the strand indicated among the flags.
 *
 * Among optional tags, we include "CV" as conversion, and it is
 * Alphanumeric with value 'A' or 'T' to show whether the C->T
 * conversion was used or the G->A (for PBAT or 2nd end of PE reads).
 */
static map_type
format_pe(const bool allow_ambig,
          const pe_result &res, const ChromLookup &cl,
          const string &read1, const string &read2,
          const string &name1, const string &name2,
          const string &cig1,  const string &cig2,
          ostream &out) {
  const bool ambig = res.ambig();
  if (!allow_ambig && ambig)
    return map_ambig;

  uint32_t r_s1 = 0, r_e1 = 0, chr1 = 0; // positions in chroms (0-based)
  uint32_t r_s2 = 0, r_e2 = 0, chr2 = 0;
  const pe_element p = res.best;

  // PE chromosomes differ or couldn't be found, treat read as unmapped
  if (!res.valid(read1.size(), read2.size()) ||
      !chrom_and_posn(cl, cig1, p.r1.pos, r_s1, r_e1, chr1) ||
      !chrom_and_posn(cl, cig2, p.r2.pos, r_s2, r_e2, chr2) || chr1 != chr2)
    return map_unmapped;

  const bool rc = p.r1.rc();

  // ADS: will this always evaluate correctly with unsigned
  // intermediate vals?
  const int tlen = rc ?
    (static_cast<int>(r_s1) - static_cast<int>(r_e2)) :
    (static_cast<int>(r_e2) - static_cast<int>(r_s1));

  // ADS: +1 to POS & PNEXT; "=" for RNEXT; "*" for QUAL
  sam_rec sr1(name1, 0, cl.names[chr1], r_s1 + 1, 255,
              cig1, "=", r_s2 + 1, tlen, read1, "*");

  sr1.add_tag("NM:i:" + to_string(p.r1.diffs));
  sr1.add_tag(p.r1.elem_is_a_rich() ? "CV:A:A" : "CV:A:T");
  set_flag(sr1, samflags::read_paired);
  set_flag(sr1, samflags::read_pair_mapped);
  set_flag(sr1, samflags::template_first);

  sam_rec sr2(name2, 0, cl.names[chr2], r_s2 + 1, 255,
              cig2, "=", r_s1 + 1, -tlen, read2, "*");

  sr2.add_tag("NM:i:" + to_string(p.r2.diffs));
  sr2.add_tag(p.r2.elem_is_a_rich() ? "CV:A:A" : "CV:A:T");
  set_flag(sr2, samflags::read_paired);
  set_flag(sr2, samflags::read_pair_mapped);
  set_flag(sr2, samflags::template_last);

  // second mate is reverse strand and richness of 1st mate
  if (rc) {
    set_flag(sr1, samflags::read_rc);
    set_flag(sr2, samflags::mate_rc);
  }
  else {
    set_flag(sr1, samflags::mate_rc);
    set_flag(sr2, samflags::read_rc);
  }

  if (allow_ambig && ambig) {
    set_flag(sr1, samflags::secondary_aln);
    set_flag(sr2, samflags::secondary_aln);
  }

  out << sr1.tostring() << "\n" << sr2.tostring() << "\n";

  return ambig ? map_ambig : map_unique;
}

struct pe_candidates {
  pe_candidates() : v(vector<se_element>(max_size)), sz(1) {}
  bool full() const {return sz == max_size;}
  bool valid(const size_t readlen) const { return v[0].valid(readlen); }
  bool valid_hit(const uint32_t readlen) const {
    return v[0].valid_hit(readlen);
  }
  void reset(const uint32_t readlen) {v.front().reset(readlen); sz = 1;}
  score_t get_cutoff() const {return v.front().diffs + 1;}
  void update(const uint32_t p, const score_t d, const flags_t s) {
    if (d < v.front().diffs) {
      if (full()) {
        std::pop_heap(begin(v), end(v));
        v.back() = se_element(p, d, s);
        std::push_heap(begin(v), end(v));
      }
      else {
        v[sz++] = se_element(p, d, s);
        std::push_heap(begin(v), begin(v) + sz);
      }
    }
  }
  bool sure_ambig(const score_t seed_number = 0) const {
    return full() && v[0].diffs <= seed_number;
  }
  bool optimal() const {
    return full() && v[0].diffs == 0;
  }

  void prepare_for_mating() {
    sort(begin(v), begin(v) + sz, // no sort_heap here as heapify used "diffs"
         [](const se_element &a, const se_element &b){return a.pos < b.pos;});
    sz = unique(begin(v), begin(v) + sz) - begin(v);
  }
  vector<se_element> v;
  uint32_t sz;
  static uint32_t max_size;
};

uint32_t pe_candidates::max_size = 20;

inline double pct(const double a, const double b) {return 100.0*a/b;}

struct se_map_stats {
  se_map_stats() :
    tot_rds(0), uniq_rds(0), ambig_rds(0), unmapped_rds(0), skipped_rds(0) {}
  uint32_t tot_rds;
  uint32_t uniq_rds;
  uint32_t ambig_rds;
  uint32_t unmapped_rds;
  uint32_t skipped_rds;

  void update(const se_result res, const string &read) {
    ++tot_rds;
    if (res.valid(read.size())) {
      const bool ambig = res.ambig();
      uniq_rds += !ambig;
      ambig_rds += ambig;
    }
    else ++unmapped_rds;
    skipped_rds += (read.length() == 0);
  }

  string tostring(const size_t n_tabs = 0) const {
    static const string tab = "    ";
    string t;
    for (size_t i = 0; i < n_tabs; ++i) t += tab;
    std::ostringstream oss;

    oss << t     << "total_reads: " << tot_rds << endl
        << t     << "mapped: " << endl
        << t+tab << "num_mapped: " << uniq_rds+ambig_rds << endl
        << t+tab << "percent_mapped: "
        << pct(uniq_rds+ambig_rds, tot_rds == 0 ? 1 : tot_rds) << endl
        << t+tab << "num_unique: " << uniq_rds << endl
        << t+tab << "percent_unique: "
        << pct(uniq_rds, tot_rds == 0 ? 1 : tot_rds) << endl
        << t+tab << "ambiguous: " << ambig_rds << endl
        << t     << "unmapped: " << unmapped_rds << endl
        << t     << "skipped: " << skipped_rds << endl;
    return oss.str();
  }
};

struct pe_map_stats {
  pe_map_stats() :
    tot_pairs(0), uniq_pairs(0), ambig_pairs(0), unmapped_pairs(0) {}
  uint32_t tot_pairs;
  uint32_t uniq_pairs;
  uint32_t ambig_pairs;
  uint32_t unmapped_pairs;
  uint32_t min_dist;
  se_map_stats end1_stats;
  se_map_stats end2_stats;

  void update(const bool allow_ambig,
              const pe_result res,
              const se_result res_se1,
              const se_result res_se2,
              const string &read1,
              const string &read2) {
    ++tot_pairs;
    const bool valid = res.valid(read1.size(), read2.size());
    const bool ambig = res.ambig();
    if (valid) {
      const bool ambig = res.ambig();
      ambig_pairs += ambig;
      uniq_pairs += !ambig;
    }

    if (!valid || (!allow_ambig && ambig)) {
      end1_stats.update(res_se1, read1);
      end2_stats.update(res_se2, read2);
    }
    else ++unmapped_pairs;
  }

  string tostring() const {
    std::ostringstream oss;
    static const string t = "    ";
    oss << "pairs:" << endl
        << t   << "total_read_pairs: " << tot_pairs << endl
        << t   << "mapped:" << endl
        << t+t << "num_mapped: " << uniq_pairs + ambig_pairs << endl
        << t+t << "percent_mapped: "
        << pct(uniq_pairs + ambig_pairs, tot_pairs) << endl
        << t+t << "num_unique: " << uniq_pairs << endl
        << t+t << "percent_unique: " << pct(uniq_pairs, tot_pairs) << endl
        << t+t << "ambiguous: " << ambig_pairs << endl
        << t   << "unmapped: " << unmapped_pairs << endl
        << "mate1:" << endl << end1_stats.tostring(1)
        << "mate2:" << endl << end2_stats.tostring(1);
    return oss.str();
  }
};

static void
select_output(const bool allow_ambig, const ChromLookup &cl,
              pe_result &best, se_result &se1, se_result &se2,
              const string &read1, const string &name1,
              const string &read2, const string &name2,
              const string &cig1, const string &cig2,
              ostream &out) {

  const map_type pe_map_type = format_pe(allow_ambig, best, cl,
                                         read1, read2, name1,
                                         name2, cig1, cig2, out);
  if (pe_map_type == map_unmapped ||
      (!allow_ambig && pe_map_type == map_ambig)) {
    if (pe_map_type == map_unmapped)
      best.reset(read1.size(), read2.size());
    if (format_se(allow_ambig, se1, cl, read1, name1, cig1, out) ==
        map_unmapped)
      se1.reset(read1.size());
    if (format_se(allow_ambig, se2, cl, read2, name2, cig2, out) ==
        map_unmapped)
      se2.reset(read2.size());
  }
}

inline bool
the_comp(const char a, const char b) {
  return (a & b) == 0;
}

score_t
full_compare(const score_t cutoff,
             Read::const_iterator read_itr,
             const Read::const_iterator read_end,
             Genome::const_iterator genome_itr) {
  score_t d = 0;
  while (d != cutoff && read_itr != read_end) {
    d += mismatch_lookup[*read_itr & *genome_itr];
    ++read_itr, ++genome_itr;
  }
  return d;
}

template <const uint16_t strand_code, class result_type>
inline void
check_hits(vector<uint32_t>::const_iterator start_idx,
           const vector<uint32_t>::const_iterator end_idx,
           const Read::const_iterator even_read_st,
           const Read::const_iterator even_read_end,
           const Read::const_iterator odd_read_st,
           const Read::const_iterator odd_read_end,
           const Genome::const_iterator genome_st,
           result_type &res) {
  for (; start_idx != end_idx && !res.sure_ambig(0); ++start_idx) {
    // GS: adds the next candidate to cache while current is compared
    __builtin_prefetch(
        &(*(genome_st + (*(start_idx + 1) >> 1)))
    );
    // ADS: (reminder) the adjustment below is because
    // 2 bases are stored in each byte
    const score_t diffs =
    (*start_idx & 1) ?
       full_compare(res.get_cutoff(), odd_read_st, odd_read_end,
                    genome_st + (*start_idx >> 1)):
       full_compare(res.get_cutoff(), even_read_st, even_read_end,
                    genome_st + (*start_idx >> 1));
    res.update(*start_idx, diffs, strand_code);
  }
}

// ADS: probably should be a lambda function for brevity
struct compare_bases {
  compare_bases(const genome_iterator g_) : g(g_) {}
  bool operator()(const uint32_t mid, const uint32_t chr) const {
    return get_bit(*(g + mid)) < chr;
  }
  const genome_iterator g;
};

template<const uint32_t seed_lim>
static void
find_candidates(const Read::const_iterator read_start,
                const genome_iterator gi,
                const uint32_t read_lim, // not necessarily read len
                vector<uint32_t>::const_iterator &low,
                vector<uint32_t>::const_iterator &high) {
  const uint32_t lim = min(read_lim, seed_lim);
  for (uint32_t p = seed::key_weight; p != lim; ++p) {
    auto first_1 = lower_bound(low, high, 1, compare_bases(gi + p));
    if (get_bit(*(read_start + p)) == 0) {
      if (first_1 == high) return; // need 0s; whole range is 0s
      high = first_1;
    }
    else {
      if (first_1 == low) return; // need 1s; whole range is 1s
      low = first_1;
    }
  }
}

template <const uint16_t strand_code, class result_type>
void
process_seeds(const uint32_t max_candidates,
              const vector<uint32_t>::const_iterator counter_st,
              const vector<uint32_t>::const_iterator index_st,
              const genome_iterator genome_st,
              const Read &read_seed,
              const Read &read_even,
              const Read &read_odd,
              vector<uint32_t> &hits,
              result_type &res) {
  const uint32_t readlen = read_seed.size();

  // used to get positions in the genome
  const auto read_start(begin(read_seed));

  // used to compare even positions in the genome
  const auto even_read_start(begin(read_even));
  const auto even_read_end(end(read_even));

  // used to compare odd positions in the genome
  const auto odd_read_start(begin(read_odd));
  const auto odd_read_end(end(read_odd));

  // n seed positions corrected for index iterval
  static const uint32_t n_specific_positions =
                        seed::n_sorting_positions - seed::index_interval + 1;

  static const uint32_t n_sensitive_positions =
                        seed::n_seed_positions - seed::index_interval + 1;

  // specific step
  hits.clear();
  uint32_t k = 0;
  get_1bit_hash(read_start, k);
  for (uint32_t j = 0; j != seed::index_interval; ++j) {
    auto s_idx(index_st + *(counter_st + k));
    auto e_idx(index_st + *(counter_st + k + 1));
    if (s_idx < e_idx) {
      find_candidates<n_specific_positions>(
        read_start + j, genome_st, readlen, s_idx, e_idx
      );
      if (hits.size() + (e_idx - s_idx) <= max_candidates){
        for(; s_idx != e_idx; ++s_idx)
          hits.push_back(*s_idx - j);
      }

      // GS: if we have too many candidates in the specific step, we will
      // also have too many candidates in the sensitive step
      else return;
    }
    shift_hash_key(*(read_start + seed::key_weight + j), k);
  }
  check_hits<strand_code>(begin(hits), end(hits),
                          even_read_start, even_read_end,
                          odd_read_start, odd_read_end,
                          genome_st.itr, res);

  // GS: stop if we found enough exact matches (1 for SE, num_candidates for PE)
  if (res.optimal()) return;

  // sensitive step
  hits.clear();

  const uint32_t shift_lim = readlen - seed::n_seed_positions;

  // GS: minimum number of shifts to cover the whole read
  const uint32_t num_shifts = readlen/seed::n_seed_positions +
                              (readlen % seed::n_seed_positions != 0);

  // assert(num_shifts > 1)
  const uint32_t shift = max(shift_lim / (num_shifts - 1), 1u);
  for (uint32_t offset = 0; offset <= shift_lim; offset += shift) {
    get_1bit_hash(read_start + offset, k);
    for (uint32_t j = 0; j != seed::index_interval; ++j, ++offset) {
      auto s_idx(index_st + *(counter_st + k));
      auto e_idx(index_st + *(counter_st + k + 1));
      if (s_idx < e_idx) {
        find_candidates<n_sensitive_positions>(
          read_start + offset, genome_st, readlen, s_idx, e_idx
        );
        if (hits.size() + (e_idx - s_idx) <= max_candidates){
          for(; s_idx != e_idx; ++s_idx)
            hits.push_back(*s_idx - offset);
        }

        // GS: seed candidates go over max candidates
        else return;
        shift_hash_key(*(read_start + seed::key_weight + offset), k);
      }
    }
    --offset;
  }
  check_hits<strand_code>(begin(hits), end(hits),
                          even_read_start, even_read_end,
                          odd_read_start, odd_read_end,
                          genome_st.itr, res);
}

template <const bool convert_a_to_g>
static void
prep_read(const string &r, Read &pread) {
  pread.resize(r.size());
  for (size_t i = 0; i != r.size(); ++i)
    pread[i] = (convert_a_to_g ?
                (encode_base_a_rich[static_cast<unsigned char>(r[i])]) :
                (encode_base_t_rich[static_cast<unsigned char>(r[i])]));
}

/* GS: this function encodes an ASCII character string into
 * two byte arrays, where each byte contains two bases, and
 * each base is represented in four bits, with active bits
 * representing the possible base matches to the genome. One
 * of the arrays(pread_even) will be used to compare the read
 * to even positions in the genome, while the other (pread_odd)
 * is used to compare odd bases. When the number of bases is odd,
 * or to account for the fact that pread_odd has a single base
 * in the first byte, reads can have a 1111 as the first or last
 * base. This will match with any base in the genome and will therefore
 * be disregarded for mismatch counting except if it aligns with Ns
 * in the genome (encoded as 0000 to mismatch with everything). These
 * cases should be unlikely in practice
 * */
static void
prep_for_seeds(const Read &pread_seed, Read &pread_even,
               Read &pread_odd) {
  static const uint8_t first_base_match_any = 0x0F;
  static const uint8_t last_base_match_any = 0xF0;
  const size_t sz = pread_seed.size();
  pread_even.resize((sz + 1)/2);
  pread_odd.resize(sz/2 + 1);

  // even encoding
  const auto s_idx(begin(pread_seed));
  const auto e_idx(end(pread_seed));
  size_t i = 0;
  for (auto it(s_idx); i != pread_even.size(); ++it, ++i) {
    pread_even[i] = *it;
    if (++it != e_idx) pread_even[i] |= ((*it) << 4);
    else pread_even[i] |= last_base_match_any;
  }

  // odd encoding
  pread_odd[0] = first_base_match_any |  ((*s_idx) << 4);
  i = 1;
  for (auto it(s_idx + 1); i != pread_odd.size(); ++it, ++i) {
    pread_odd[i] = *it;
    if (++it != e_idx) pread_odd[i] |= ((*it) << 4);
    else pread_odd[i] |= last_base_match_any;
  }
}

/* this lookup improves speed when running alignment because we do not
   have to check if bases are equal or different */
inline score_t
mismatch_score(const char q_base, const uint8_t t_base) {
  return simple_local_alignment::score_lookup[the_comp(q_base, t_base)];
}

static score_t
count_deletions(const string &cigar) {
  score_t ans = 0;
  for (auto it(begin(cigar)); it != end(cigar);) {
    ans += (extract_op_count(it, end(cigar))) * (*(it++) == 'D');
  }
  return ans;
}

/* GS: does not consider soft-clipping into edit distance
 * but requires a minimum alignment length. The way to convert
 * from alignment to edit distance is as follows: Let
 *  - M be the number of matches
 *  - m be the number of mismatches
 *  - I be the number of insertions
 *  - D be the number of deletions
 *  - S be the score
 *  - L be the alignment length
 *  - E be the edit distance.
 *  then, for a 1 -1 -1 scoring system we have:
 *  { S = M - m - I - D
 *  { L = M + m + I
 *  { E = m + I + D
 *  whence: E = (L - S + D) / 2,
 *  wich means we have to correct for the number of deletions using
 *  the cigar to make a proper conversion */
static score_t
edit_distance(const score_t aln_score, const size_t len, const string cigar) {
  if (len < se_element::min_aligned_length)
    return numeric_limits<score_t>::max();

  // GS: this conversion only works for 1 -1 -1
  return (static_cast<score_t>(len) - aln_score + count_deletions(cigar)) / 2;
}

using AbismalAlignSimple = AbismalAlign<mismatch_score,
                                        simple_local_alignment::indel>;

/* This function alings the read and converts "diffs" from Hamming
 * distance to edit distance */
static void
align_read(se_element &res, string &cigar, const Read &pread,
           AbismalAlignSimple &aln) {
  // ends early if mismatches are good enough
  if (res.valid(pread.size()))
    cigar = to_string(pread.size()) + "M";

  /* This condition ensures the local alignment will not degenerate
   * to an empty alignment because there is at least one match. If
   * this is not the case the read is not valid, and we don't need
   * to make a cigar for it because it will not be output as long
   * as max_hits < min_read_length */
  else if (res.diffs < static_cast<score_t>(pread.size())) {
    uint32_t len = 0; // the region of the read the alignment spans
    const score_t aln_score = aln.align(pread, res.pos, len, cigar);
    res.diffs = edit_distance(aln_score, len, cigar);
  }
  else {
    cigar = to_string(pread.size()) + "M";
  }
}

/* This is the same function as above, but when we only have a
 * se_result and we don't have the originally converted pread, so we
 * have to look at the se_element to know how to convert it back */
static void
align_read(se_element &res, string &cigar, const string &read,
           Read &pread, AbismalAlignSimple &aln) {
  // ends early if mismatches are good enough
  if (res.valid(read.size()))
    cigar = to_string(read.size()) + "M";

  else if (res.diffs < static_cast<score_t>(read.size())) {
    uint32_t len = 0; // the region of the read the alignment spans
    // re-encodes the read based on best match
    if (res.rc()) {
      const string read_rc(revcomp(read));
      // rc reverses richness of read
      if (res.elem_is_a_rich()) prep_read<false>(read_rc, pread);
      else prep_read<true>(read_rc, pread);
    }
    else {
      if (res.elem_is_a_rich()) prep_read<true>(read, pread);
      else prep_read<false>(read, pread);
    }
    const score_t aln_score = aln.align(pread, res.pos, len, cigar);
    res.diffs = edit_distance(aln_score, len, cigar);
  }
  else {
    cigar = to_string(pread.size()) + "M";
  }
}

static void
align_se_candidates(se_result &res, string &cigar,
                    const string &read, Read &pread,
                    AbismalAlignSimple &aln) {
  if (res.best.valid_hit(read.size())) {
    align_read(res.best, cigar, read, pread, aln);
    // second best is only a valid hit if best also is
    if (res.second_best.valid_hit(read.size())) {
      string tmp_cigar;
      align_read(res.second_best, tmp_cigar, read, pread, aln);

      // if second best is better than the best, we update the cigar
      if (res.sort_candidates())
        cigar = tmp_cigar;
    }
  }
}

template <const  conversion_type conv>
inline void
map_single_ended(const bool VERBOSE,
                 const bool allow_ambig,
                 const size_t batch_size,
                 const size_t max_candidates,
                 const vector<uint32_t>::const_iterator counter_st,
                 const vector<uint32_t>::const_iterator index_st,
                 const genome_iterator genome_st,
                 const ChromLookup &cl,
                 ReadLoader &rl,
                 omp_lock_t &read_lock,
                 omp_lock_t &write_lock,
                 se_map_stats &se_stats,
                 ostream &out,
                 ProgressBar &progress) {
  vector<string> names;
  vector<string> reads;
  vector<string> cigar;

  vector<se_result> res;

  names.reserve(batch_size);
  reads.reserve(batch_size);

  cigar.resize(batch_size);
  res.resize(batch_size);

  Read pread, pread_even, pread_odd;
  vector<uint32_t> hits;
  hits.reserve(max_candidates);

  while (rl.good()) {
    omp_set_lock(&read_lock);
    if (VERBOSE && progress.time_to_report(rl.get_current_byte()))
      progress.report(cerr, rl.get_current_byte());

    rl.load_reads(names, reads);
    omp_unset_lock(&read_lock);

    size_t max_batch_read_length = 0;
    update_max_read_length(max_batch_read_length, reads);

    AbismalAlignSimple aln(genome_st, max_batch_read_length);

    const size_t n_reads = reads.size();
    for (size_t i = 0; i < n_reads; ++i) {
      res[i].reset(reads[i].size());
      if (!reads[i].empty()) {
        prep_read<conv>(reads[i], pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('+', conv)>(max_candidates,
                                                  counter_st, index_st,
                                                  genome_st, pread,
                                                  pread_even, pread_odd,
                                                  hits, res[i]);

        const string read_rc(revcomp(reads[i]));
        prep_read<!conv>(read_rc, pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('-', conv)>(max_candidates,
                                                  counter_st, index_st,
                                                  genome_st, pread,
                                                  pread_even, pread_odd,
                                                  hits, res[i]);
        align_se_candidates(res[i], cigar[i], reads[i], pread, aln);
      }
    }

    omp_set_lock(&write_lock);
    for (size_t i = 0; i < n_reads; ++i) {
      if (format_se(allow_ambig, res[i], cl, reads[i], names[i], cigar[i], out)
          == map_unmapped)
        res[i].reset(reads[i].size());
      se_stats.update(res[i], reads[i]);
    }
    omp_unset_lock(&write_lock);
  }
}

inline void
map_single_ended_rand(const bool VERBOSE,
                      const bool allow_ambig,
                      const size_t batch_size,
                      const size_t max_candidates,
                      const vector<uint32_t>::const_iterator counter_st,
                      const vector<uint32_t>::const_iterator index_st,
                      const genome_iterator genome_st,
                      const ChromLookup &cl,
                      ReadLoader &rl,
                      omp_lock_t &read_lock,
                      omp_lock_t &write_lock,
                      se_map_stats &se_stats,
                      ostream &out,
                      ProgressBar &progress) {
  vector<string> names;
  vector<string> reads;
  vector<string> cigar;
  vector<se_result> res;

  names.reserve(batch_size);
  reads.reserve(batch_size);
  cigar.resize(batch_size);
  res.resize(batch_size);

  Read pread, pread_even, pread_odd;
  vector<uint32_t> hits;
  hits.reserve(max_candidates);

  while (rl.good()) {
    omp_set_lock(&read_lock);
    if (VERBOSE && progress.time_to_report(rl.get_current_byte()))
      progress.report(cerr, rl.get_current_byte());
    rl.load_reads(names, reads);
    omp_unset_lock(&read_lock);

    size_t max_batch_read_length = 0;
    update_max_read_length(max_batch_read_length, reads);

    AbismalAlignSimple aln(genome_st, max_batch_read_length);
    const size_t n_reads = reads.size();
    for (size_t i = 0; i < n_reads; ++i) {
      res[i].reset(reads[i].size());
      if (!reads[i].empty()) {
        prep_read<t_rich>(reads[i], pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('+', t_rich)>(max_candidates,
                                                    counter_st, index_st,
                                                    genome_st, pread,
                                                    pread_even, pread_odd,
                                                    hits, res[i]);
        prep_read<a_rich>(reads[i], pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('+', a_rich)>(max_candidates,
                                                    counter_st, index_st,
                                                    genome_st, pread,
                                                    pread_even, pread_odd,
                                                    hits, res[i]);

        const string read_rc(revcomp(reads[i]));
        prep_read<t_rich>(read_rc, pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('-', a_rich)>(max_candidates,
                                                    counter_st, index_st,
                                                    genome_st, pread,
                                                    pread_even, pread_odd,
                                                    hits, res[i]);

        prep_read<a_rich>(read_rc, pread);
        prep_for_seeds(pread, pread_even, pread_odd);
        process_seeds<get_strand_code('-', t_rich)>(max_candidates,
                                                    counter_st, index_st,
                                                    genome_st, pread,
                                                    pread_even, pread_odd,
                                                    hits, res[i]);
        align_se_candidates(res[i], cigar[i], reads[i], pread, aln);
      }
    }

    omp_set_lock(&write_lock);
    for (size_t i = 0; i < n_reads; ++i) {
      if (format_se(allow_ambig, res[i], cl, reads[i], names[i], cigar[i], out)
          == map_unmapped)
        res[i].reset(reads[i].size());
      se_stats.update(res[i], reads[i]);
    }
    omp_unset_lock(&write_lock);
  }
}

template <const conversion_type conv, const bool random_pbat>
void
run_single_ended(const bool VERBOSE,
                 const bool allow_ambig,
                 const string &reads_file,
                 const size_t batch_size,
                 const size_t max_candidates,
                 const AbismalIndex &abismal_index,
                 se_map_stats &se_stats,
                 ostream &out) {
  const auto counter_st(begin(abismal_index.counter));
  const auto index_st(begin(abismal_index.index));
  const genome_iterator genome_st(begin(abismal_index.genome));

  ReadLoader rl(reads_file, batch_size);
  ProgressBar progress(get_filesize(reads_file), "mapping reads");

  omp_lock_t read_lock;
  omp_lock_t write_lock;

  omp_init_lock(&read_lock);
  omp_init_lock(&write_lock);

  if (VERBOSE)
    progress.report(cerr, 0);

  double start_time = omp_get_wtime();
  if (VERBOSE && progress.time_to_report(rl.get_current_byte()))
    progress.report(cerr, rl.get_current_byte());

#pragma omp parallel for
  for (int i = 0; i < omp_get_num_threads(); ++i) {
    if (random_pbat)
      map_single_ended_rand(VERBOSE, allow_ambig, batch_size,
          max_candidates, counter_st, index_st, genome_st, abismal_index.cl,
          rl, read_lock, write_lock, se_stats, out, progress);
    else
      map_single_ended<conv>(VERBOSE, allow_ambig, batch_size,
          max_candidates, counter_st, index_st, genome_st, abismal_index.cl,
          rl, read_lock, write_lock, se_stats, out, progress);

  }

  if (VERBOSE) {
    progress.report(cerr, get_filesize(reads_file));
    cerr << "[total mapping time: " << omp_get_wtime() - start_time << "]"
         << endl;
  }
}

static void
best_single(const pe_candidates &pres, se_result &res) {
  const auto lim(begin(pres.v) + pres.sz);

  // get best and second best by mismatch
  for (auto i(begin(pres.v)); i != lim && !res.sure_ambig(0); ++i)
    res.update(i->pos, i->diffs, i->flags);
}

template <const bool swap_ends>
static void
best_pair(const pe_candidates &res1, const pe_candidates &res2,
          const Read &pread1, const Read &pread2,
          string &cig1, string &cig2,
          AbismalAlignSimple &aln, pe_result &best) {

  auto j1 = begin(res1.v);
  const auto j1_end = j1 + res1.sz;
  const auto j2_end = begin(res2.v) + res2.sz;
  se_element s1, s2;
  string cand_cig1, cand_cig2;

  for (auto j2(begin(res2.v)); j2 != j2_end; ++j2) {
    s2 = *j2;
    if (s2.valid_hit(pread2.size())){
      const uint32_t unaligned_lim = s2.pos + pread2.size();
      for (j1 = begin(res1.v); j1 != j1_end &&
           j1->pos + pe_element::max_dist < unaligned_lim; ++j1);

      bool aligned_s2 = false;
      uint32_t aligned_lim;
      while (j1 != j1_end && j1->pos + pe_element::min_dist <= unaligned_lim) {
        if (j1->valid_hit(pread1.size())) {
          s1 = *j1;
          align_read(s1, cand_cig1, pread1, aln);

          // GS: guarantees that s2 is aligned only once
          if (!aligned_s2) {
            align_read(s2, cand_cig2, pread2, aln);
            aligned_lim = s2.pos + cigar_rseq_ops(cand_cig2);
            aligned_s2 = true;
          }

          // GS: only accept if length post alignment is still within limits
          if ((s1.pos + pe_element::max_dist >= aligned_lim) &&
              (s1.pos + pe_element::min_dist <= aligned_lim)) {
            const pe_element p(swap_ends ? s2 : s1, swap_ends ? s1 : s2);
            if (best.update(p)) {
              cig1 = cand_cig1;
              cig2 = cand_cig2;
            }
          }
        }
        ++j1;
      }
    }
  }
}

template <const bool swap_ends>
inline void
select_maps(const Read &pread1, const Read &pread2,
            string &cig1, string &cig2,
            pe_candidates &res1, pe_candidates &res2,
            se_result &res_se1, se_result &res_se2,
            AbismalAlignSimple &aln, pe_result &best) {
  res1.prepare_for_mating();
  res2.prepare_for_mating();

  best_pair<swap_ends>(res1, res2, pread1, pread2, cig1, cig2, aln, best);

  // if PE should not be reported, try to find the best single
  best_single(res1, res_se1);
  best_single(res2, res_se2);
}

template <const bool cmp, const bool swap_ends,
          const uint16_t strand_code1, const uint16_t strand_code2>
inline void
map_fragments(const string &read1, const string &read2,
             Read &pread1,  Read &pread2,
             Read &pread_even, Read &pread_odd,
             string &cigar1, string &cigar2,
             const uint32_t max_candidates,
             const vector<uint32_t>::const_iterator counter_st,
             const vector<uint32_t>::const_iterator index_st,
             const genome_iterator genome_st,
             AbismalAlignSimple &aln,
             vector<uint32_t> &hits,
             pe_candidates &res1, pe_candidates &res2,
             se_result &res_se1, se_result &res_se2,
             pe_result &bests) {
  res1.reset(read1.size());
  res2.reset(read2.size());

  if (!read1.empty()) {
    prep_read<cmp>(read1, pread1);
    prep_for_seeds(pread1, pread_even, pread_odd);
    process_seeds<strand_code1>(max_candidates, counter_st, index_st, genome_st,
                                pread1, pread_even, pread_odd, hits, res1);
  }

  if (!read2.empty()) {
    const string read_rc(revcomp(read2));
    prep_read<cmp>(read_rc, pread2);
    prep_for_seeds(pread2, pread_even, pread_odd);
    process_seeds<strand_code2>(max_candidates, counter_st, index_st, genome_st,
                                pread2, pread_even, pread_odd, hits, res2);
  }

  select_maps<swap_ends>(pread1, pread2, cigar1, cigar2,
                         res1, res2, res_se1, res_se2, aln, bests);
}

template <const conversion_type conv>
void
map_paired_ended(const bool VERBOSE,
                 const bool allow_ambig,
                 const size_t batch_size,
                 const size_t max_candidates,
                 const vector<uint32_t>::const_iterator counter_st,
                 const vector<uint32_t>::const_iterator index_st,
                 const genome_iterator genome_st,
                 const ChromLookup &cl,
                 ReadLoader &rl1, ReadLoader &rl2,
                 omp_lock_t &read_lock, omp_lock_t &write_lock,
                 pe_map_stats &pe_stats, ostream &out,
                 ProgressBar &progress) {
  vector<string> names1, reads1, cigar1;
  vector<string> names2, reads2, cigar2;

  vector<pe_result> bests;
  vector<pe_candidates> res1, res2;
  vector<se_result> res_se1, res_se2;

  names1.reserve(batch_size);
  reads1.reserve(batch_size);
  cigar1.resize(batch_size);

  names2.reserve(batch_size);
  reads2.reserve(batch_size);
  cigar2.resize(batch_size);

  bests.resize(batch_size);

  res1.resize(batch_size);
  res2.resize(batch_size);

  res_se1.resize(batch_size);
  res_se2.resize(batch_size);

  vector<uint32_t> hits;
  hits.reserve(max_candidates);

  Read pread1, pread2, pread_even, pread_odd;

  while (rl1.good() && rl2.good()) {
    omp_set_lock(&read_lock);

    if (VERBOSE && progress.time_to_report(rl1.get_current_byte()))
      progress.report(cerr, rl1.get_current_byte());

    rl1.load_reads(names1, reads1);
    rl2.load_reads(names2, reads2);
    omp_unset_lock(&read_lock);

    size_t max_batch_read_length = 0;
    update_max_read_length(max_batch_read_length, reads1);
    update_max_read_length(max_batch_read_length, reads2);

    AbismalAlignSimple aln(genome_st, max_batch_read_length);

    const size_t n_reads = reads1.size();
    for (size_t i = 0 ; i < n_reads; ++i) {
      res_se1[i].reset(reads1[i].size());
      res_se2[i].reset(reads2[i].size());
      bests[i].reset(reads1[i].size(), reads2[i].size());

      map_fragments<conv, false,
                   get_strand_code('+',conv),
                   get_strand_code('-', flip_conv(conv))>(
         reads1[i], reads2[i], pread1, pread2, pread_even, pread_odd,
         cigar1[i], cigar2[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res1[i], res2[i], res_se1[i], res_se2[i], bests[i]
      );

      map_fragments<!conv, true,
                   get_strand_code('+', flip_conv(conv)),
                   get_strand_code('-', conv)>(
         reads2[i], reads1[i], pread1, pread2, pread_even, pread_odd,
         cigar2[i], cigar1[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res2[i], res1[i], res_se2[i], res_se1[i], bests[i]
      );

      if (!bests[i].valid(reads1[i].size(), reads2[i].size()) ||
          bests[i].ambig()) {
        align_se_candidates(res_se1[i], cigar1[i], reads1[i], pread1, aln);
        align_se_candidates(res_se2[i], cigar2[i], reads2[i], pread2, aln);
      }
    }

    omp_set_lock(&write_lock);
    for (size_t i = 0; i < n_reads; ++i) {
      select_output(allow_ambig, cl, bests[i], res_se1[i], res_se2[i],
                    reads1[i], names1[i], reads2[i], names2[i],
                    cigar1[i], cigar2[i], out);
      pe_stats.update(allow_ambig, bests[i], res_se1[i], res_se2[i],
                    reads1[i], reads2[i]);
    }
    omp_unset_lock(&write_lock);
  }
}

static void
map_paired_ended_rand(const bool VERBOSE,
                 const bool allow_ambig,
                 const size_t batch_size,
                 const size_t max_candidates,
                 const vector<uint32_t>::const_iterator counter_st,
                 const vector<uint32_t>::const_iterator index_st,
                 const genome_iterator genome_st,
                 const ChromLookup &cl,
                 ReadLoader &rl1, ReadLoader &rl2,
                 omp_lock_t &read_lock, omp_lock_t &write_lock,
                 pe_map_stats &pe_stats, ostream &out,
                 ProgressBar &progress) {
  vector<string> names1, reads1, cigar1;
  vector<string> names2, reads2, cigar2;

  vector<pe_result> bests;
  vector<pe_candidates> res1, res2;
  vector<se_result> res_se1, res_se2;

  names1.reserve(batch_size);
  reads1.reserve(batch_size);
  cigar1.resize(batch_size);

  names2.reserve(batch_size);
  reads2.reserve(batch_size);
  cigar2.resize(batch_size);

  bests.resize(batch_size);

  res1.resize(batch_size);
  res2.resize(batch_size);

  res_se1.resize(batch_size);
  res_se2.resize(batch_size);

  vector<uint32_t> hits;
  hits.reserve(max_candidates);

  Read pread1, pread2, pread_even, pread_odd;

  while (rl1.good() && rl2.good()) {
    omp_set_lock(&read_lock);

    if (VERBOSE && progress.time_to_report(rl1.get_current_byte()))
      progress.report(cerr, rl1.get_current_byte());

    rl1.load_reads(names1, reads1);
    rl2.load_reads(names2, reads2);
    omp_unset_lock(&read_lock);

    size_t max_batch_read_length = 0;
    update_max_read_length(max_batch_read_length, reads1);
    update_max_read_length(max_batch_read_length, reads2);

    AbismalAlignSimple aln(genome_st, max_batch_read_length);

    const size_t n_reads = reads1.size();
    for (size_t i = 0 ; i < n_reads; ++i) {
      res_se1[i].reset(reads1[i].size());
      res_se2[i].reset(reads2[i].size());
      bests[i].reset(reads1[i].size(), reads2[i].size());

      // GS: (1) T/A-rich +/- strand
      map_fragments<t_rich, false,
                   get_strand_code('+', t_rich),
                   get_strand_code('-', a_rich)>(
         reads1[i], reads2[i], pread1, pread2, pread_even, pread_odd,
         cigar1[i], cigar2[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res1[i], res2[i], res_se1[i], res_se2[i], bests[i]
      );

      // GS: (2) T/A-rich, -/+ strand
      map_fragments<a_rich, true,
                   get_strand_code('+', a_rich),
                   get_strand_code('-', t_rich)>(
         reads2[i], reads1[i], pread2, pread1, pread_even, pread_odd,
         cigar2[i], cigar1[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res2[i], res1[i], res_se2[i], res_se1[i], bests[i]
      );

      // GS: (3) A/T-rich +/- strand
      map_fragments<a_rich, false,
                   get_strand_code('+', a_rich),
                   get_strand_code('-', t_rich)>(
         reads1[i], reads2[i], pread1, pread2, pread_even, pread_odd,
         cigar1[i], cigar2[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res1[i], res2[i], res_se1[i], res_se2[i], bests[i]
      );

      // GS: (4) A/T-rich, -/+ strand
      map_fragments<t_rich, true,
                   get_strand_code('+', t_rich),
                   get_strand_code('-', a_rich)>(
         reads2[i], reads1[i], pread2, pread1, pread_even, pread_odd,
         cigar2[i], cigar1[i], max_candidates, counter_st, index_st, genome_st,
         aln, hits, res2[i], res1[i], res_se2[i], res_se1[i], bests[i]
      );

      // GS: align best SE candidates if no concordant pairs found
      if (!bests[i].valid(reads1[i].size(), reads2[i].size()) ||
           bests[i].ambig()) {
        align_se_candidates(res_se1[i], cigar1[i], reads1[i], pread1, aln);
        align_se_candidates(res_se2[i], cigar2[i], reads2[i], pread2, aln);
      }
    }

    omp_set_lock(&write_lock);
    for (size_t i = 0; i < n_reads; ++i) {
      select_output(allow_ambig, cl, bests[i], res_se1[i], res_se2[i],
                        reads1[i], names1[i], reads2[i], names2[i],
                        cigar1[i], cigar2[i], out);

      pe_stats.update(allow_ambig, bests[i], res_se1[i], res_se2[i],
                      reads1[i], reads2[i]);
    }
    omp_unset_lock(&write_lock);

  }
}

template <const conversion_type conv, const bool random_pbat>
void
run_paired_ended(const bool VERBOSE,
                 const bool allow_ambig,
                 const string &reads_file1,
                 const string &reads_file2,
                 const size_t batch_size,
                 const size_t max_candidates,
                 const AbismalIndex &abismal_index,
                 pe_map_stats &pe_stats,
                 ostream &out) {
  const auto counter_st(begin(abismal_index.counter));
  const auto index_st(begin(abismal_index.index));
  const genome_iterator genome_st(begin(abismal_index.genome));

  ReadLoader rl1(reads_file1, batch_size);
  ReadLoader rl2(reads_file2, batch_size);
  ProgressBar progress(get_filesize(reads_file1), "mapping reads");

  omp_lock_t read_lock;
  omp_lock_t write_lock;

  omp_init_lock(&read_lock);
  omp_init_lock(&write_lock);

  if (VERBOSE)
    progress.report(cerr, 0);

  double start_time = omp_get_wtime();

#pragma omp parallel for
  for (int i = 0; i < omp_get_num_threads(); ++i) {
    if (random_pbat)
      map_paired_ended_rand(VERBOSE, allow_ambig, batch_size,
          max_candidates, counter_st, index_st, genome_st, abismal_index.cl,
          rl1, rl2, read_lock, write_lock, pe_stats, out, progress);
    else
      map_paired_ended<conv>(VERBOSE, allow_ambig, batch_size,
          max_candidates, counter_st, index_st, genome_st, abismal_index.cl,
          rl1, rl2, read_lock, write_lock, pe_stats, out, progress);
  }

  if (VERBOSE) {
    progress.report(cerr, get_filesize(reads_file1));
    cerr << "[total mapping time: " << omp_get_wtime() - start_time << "]" << endl;
  }
}

static void
select_max_candidates(const bool sensitive_mode,
                      const uint32_t genome_size,
                      uint32_t &max_candidates) {
  static const double max_frac_default = 1e-5;
  static const double max_frac_sensitive = 1.0;
  const double genome_frac =
    sensitive_mode ? max_frac_sensitive : max_frac_default;

  // GS: the max_max_candidates avoids super large reserves
  // on the hits() vector
  static const uint32_t min_max_candidates = 100u;
  static const uint32_t max_max_candidates = 1e6;

  const uint32_t c = static_cast<uint32_t>(genome_size * genome_frac);
  max_candidates = max(c, min_max_candidates);
  max_candidates = min(c, max_max_candidates);
}

int main(int argc, const char **argv) {

  try {
    static const string ABISMAL_VERSION = "0.1.2";
    string index_file;
    string outfile;
    string stats_outfile = "";
    bool VERBOSE = false;
    bool GA_conversion = false;
    bool allow_ambig = false;
    bool pbat_mode = false;
    bool random_pbat = false;
    bool sensitive_mode = false;
    uint32_t max_candidates = 0;
    size_t batch_size = 20000;
    int n_threads = 1;

    /****************** COMMAND LINE OPTIONS ********************/
    OptionParser opt_parse(strip_path(argv[0]),
                           "map bisulfite converted reads",
                           "<reads-fq1> [<reads-fq2>]");
    opt_parse.set_show_defaults();
    opt_parse.add_opt("index", 'i', "index file", true, index_file);
    opt_parse.add_opt("outfile", 'o', "output file", false, outfile);
    opt_parse.add_opt("mapstats", 'm',
                      "mapstats output file. If not provided, it "
                      "will be generated as .mapstats suffix to the "
                      "output file name", false, stats_outfile);
    opt_parse.add_opt("threads", 't', "number of threads", false, n_threads);
    opt_parse.add_opt("batch", 'b', "reads to load at once",
                      false, batch_size);
    opt_parse.add_opt("candidates", 'c', "max candidates for full comparison",
                      false, max_candidates);
    opt_parse.add_opt("sensitive", 's', "run abismal on max sensitivity mode",
                      false, sensitive_mode);
    opt_parse.add_opt("max-mates", 'p', "max candidates as mates (pe mode)",
                      false, pe_candidates::max_size);
    opt_parse.add_opt("min-frag", 'l', "min fragment size (pe mode)",
                      false, pe_element::min_dist);
    opt_parse.add_opt("max-frag", 'L', "max fragment size (pe mode)",
                      false, pe_element::max_dist);
    opt_parse.add_opt("max-frag", 'M', "max fractional edit distance",
                      false, se_element::valid_frac);
    opt_parse.add_opt("ambig", 'a', "report a posn for ambiguous mappers",
                      false, allow_ambig);
    opt_parse.add_opt("pbat", 'P', "input follows the PBAT protocol",
                      false, pbat_mode);
    opt_parse.add_opt("random-pbat", 'R', "input follows random PBAT protocol",
                      false, random_pbat);
    opt_parse.add_opt("a-rich", 'A', "indicates reads are a-rich (se mode)",
                      false, GA_conversion);
    opt_parse.add_opt("verbose", 'v', "print more run info", false, VERBOSE);
    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (argc == 1 || opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (leftover_args.size() != 1 && leftover_args.size() != 2) {
      cerr << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    if (outfile.empty() && stats_outfile.empty()) {
      cerr << "please provide a file name for either the .sam"
           << "output (-o flag) or the stats file (-m flag)" << endl;

      return EXIT_SUCCESS;
    }
    if (n_threads <= 0) {
      cerr << "please choose a positive number of threads" << endl;
      return EXIT_SUCCESS;
    }
    const string reads_file = leftover_args.front();
    string reads_file2;
    bool paired_end = false;
    if (leftover_args.size() == 2) {
      paired_end = true;
      reads_file2 = leftover_args.back();
    }
    /****************** END COMMAND LINE OPTIONS *****************/

    omp_set_num_threads(n_threads);
    AbismalIndex::VERBOSE = VERBOSE;

    if (VERBOSE)
      cerr << "[loading abismal index]" << endl;
    AbismalIndex abismal_index;
    const double start_time = omp_get_wtime();
    abismal_index.read(index_file);

    if (VERBOSE) {
      cerr << "[loading time: " << (omp_get_wtime() - start_time) << "]" << endl;
      cerr << "[using " << n_threads << " threads for mapping]\n";
      if (paired_end)
        cerr << "[mapping paired end: " << reads_file << " "
             << reads_file2 << "]\n";
      else
        cerr << "[mapping single end: " << reads_file << "]\n";
      cerr << "[output file: " << outfile << "]" << endl;
      if (sensitive_mode)
        cerr << "[running abismal on sensitive mode]\n";
    }

    // avoiding opening the stats output file until mapping is done
    se_map_stats se_stats;
    pe_map_stats pe_stats;

    std::ofstream of;
    if (!outfile.empty()) of.open(outfile.c_str(), std::ios::binary);
    std::ostream out(outfile.empty() ? std::cout.rdbuf() : of.rdbuf());

    if (!out)
      throw runtime_error("failed to open output file: " + outfile);

    write_sam_header(abismal_index.cl.names, abismal_index.cl.starts,
                     "ABISMAL", ABISMAL_VERSION, argc, argv, out);

    if (max_candidates == 0)
      select_max_candidates(sensitive_mode, abismal_index.cl.get_genome_size(),
                            max_candidates);

    if (reads_file2.empty()) {
      if (GA_conversion || pbat_mode)
        run_single_ended<a_rich, false>(VERBOSE, allow_ambig, reads_file,
            batch_size, max_candidates, abismal_index, se_stats, out);
      else if (random_pbat)
        run_single_ended<t_rich, true>(VERBOSE, allow_ambig, reads_file,
            batch_size, max_candidates, abismal_index, se_stats, out);
      else
        run_single_ended<t_rich, false>(VERBOSE, allow_ambig, reads_file,
            batch_size, max_candidates, abismal_index, se_stats, out);
    }
    else {
      if (pbat_mode)
        run_paired_ended<a_rich, false>(VERBOSE, allow_ambig, reads_file,
            reads_file2, batch_size, max_candidates, abismal_index, pe_stats,
            out);
      else if (random_pbat)
        run_paired_ended<t_rich, true>(VERBOSE,  allow_ambig, reads_file,
            reads_file2, batch_size, max_candidates, abismal_index, pe_stats,
            out);
      else
        run_paired_ended<t_rich, false>(VERBOSE, allow_ambig, reads_file,
            reads_file2, batch_size, max_candidates, abismal_index, pe_stats,
            out);
    }
    std::ofstream stat_out(stats_outfile.empty() ?
                           (outfile + ".mapstats") : stats_outfile);
    stat_out << (reads_file2.empty() ?
                 se_stats.tostring() : pe_stats.tostring());
  }
  catch (const runtime_error &e) {
    cerr << e.what() << endl;
    return EXIT_FAILURE;
  }
  catch (std::bad_alloc &ba) {
    cerr << "ERROR: could not allocate memory" << endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
