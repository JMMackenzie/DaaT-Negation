#ifndef INVIDX_HPP
#define INVIDX_HPP

#include "query.hpp"
#include "sdsl/config.hpp"
#include "sdsl/int_vector.hpp"
#include "block_postings_list.hpp"
#include "util.hpp"
#include "generic_rank.hpp"
#include "bm25.hpp"
#include "impact.hpp"
#include <unordered_set>

// Output the heap threshold at every scored document
//#define HORIZON

// Fill profile numbers
//#define PROFILE

#ifdef PROFILE

  uint64_t qry_id = 0;
  uint64_t postings_evaluated = 0;
  uint64_t docs_fully_evaluated = 0;
  uint64_t docs_added_to_heap = 0; 
  double final_threshold = 0; 
  uint64_t negation_passed = 0;
  uint64_t negation_failed = 0;
  std::unordered_set<uint64_t> unique_pivots;
#endif

using namespace sdsl;

template<class t_pl = block_postings_list<128>,
         class t_rank = generic_rank>
class idx_invfile {
public:
  using size_type = sdsl::int_vector<>::size_type;
  using plist_type = t_pl;
  using ranker_type = t_rank;
private:
  // determine lists
  struct plist_wrapper {
    typename plist_type::const_iterator cur;
    typename plist_type::const_iterator end;
    double list_max_score;
    double f_t;
    plist_wrapper() = default;
    plist_wrapper(plist_type& pl) {
      f_t = pl.size(); 
      cur = pl.begin();
      end = pl.end();
      list_max_score = pl.list_max_score();
    }
  };
private:
  std::vector<plist_type> m_postings_lists;
  std::unique_ptr<ranker_type> ranker;

public:
  idx_invfile() = default;
  double m_F;
  double m_conjunctive_max;

  // Search constructor 
  idx_invfile(std::string& postings_file, const double F) : m_F(F)
  {
    
    std:: ifstream ifs(postings_file);
    if (ifs.is_open() != true){
      std::cerr << "Could not open file: " <<  postings_file << std::endl;
      exit(EXIT_FAILURE);
    }
    size_t num_lists; 
    read_member(num_lists,ifs);
    m_postings_lists.resize(num_lists);
    for (size_t i=0;i<num_lists;i++) {
      m_postings_lists[i].load(ifs);
    }
  }

  auto serialize(std::ostream& out, 
                 sdsl::structure_tree_node* v=NULL, 
                 std::string name="") const -> size_type {

    structure_tree_node* child = structure_tree::add_child(v, name, 
                                 util::class_name(*this));
    size_type written_bytes = 0;
    size_t num_lists = m_postings_lists.size();
    written_bytes += sdsl::serialize(num_lists,out,child,"num postings lists");
    for (const auto& pl : m_postings_lists) {
      written_bytes += sdsl::serialize(pl,out,child,"postings list");
    }
    structure_tree::add_size(child, written_bytes);
    return written_bytes;
  }

  // Loads the ranker data
  void load(std::vector<uint64_t> doc_len, uint64_t terms, uint64_t num_docs,
            postings_form postings_type){
    if (postings_type == FREQUENCY) {
      ranker = std::unique_ptr<ranker_type>(new rank_bm25(doc_len, terms, num_docs));
    }
    else if (postings_type == QUANTIZED) {
      ranker = std::unique_ptr<ranker_type>(new rank_impact);
    }
  }

  // Finds the posting with the least number of items remaining other than
  // the current ID
  typename std::vector<plist_wrapper*>::iterator
  find_shortest_list(std::vector<plist_wrapper*>& postings_lists,
                     const typename std::vector<plist_wrapper*>::iterator& end,
                     const uint64_t id) 
  {
    auto itr = postings_lists.begin();
    if (itr != end) {
      size_t smallest = std::numeric_limits<size_t>::max();
      auto smallest_itr = itr;
      while (itr != end) {
        if ((*itr)->cur.remaining() < smallest && (*itr)->cur.docid() != id) {
          smallest = (*itr)->cur.remaining();
          smallest_itr = itr;
        }
        ++itr;
      }
      return smallest_itr;
    }
    return end;
  }

  void sort_list_by_id(std::vector<plist_wrapper*>& plists) {
    // delete if necessary
    auto del_itr = plists.begin();
    while (del_itr != plists.end()) {
      if ((*del_itr)->cur == (*del_itr)->end) {
        del_itr = plists.erase(del_itr);
      } else {
        del_itr++;
      }
    }
    // sort
    auto id_sort = [](const plist_wrapper* a,const plist_wrapper* b) {
      return a->cur.docid() < b->cur.docid();
    };
    std::sort(plists.begin(),plists.end(),id_sort);
  }

  // WAND-Forwarding: Forwards smallest list to provided ID
  void forward_lists(std::vector<plist_wrapper*>& postings_lists,
       const typename std::vector<plist_wrapper*>::iterator& pivot_list,
       const uint64_t id) {

    auto smallest_itr = find_shortest_list(postings_lists,pivot_list,id);
    // advance the smallest list to the new id
    (*smallest_itr)->cur.skip_to_id(id);

    if ((*smallest_itr)->cur == (*smallest_itr)->end) {
      // list is finished! reorder list by id
      sort_list_by_id(postings_lists);
      return;
    }

    // bubble it down!
    auto next = smallest_itr + 1;
    auto list_end = postings_lists.end();
    while (next != list_end && 
           (*smallest_itr)->cur.docid() > (*next)->cur.docid()) {
      std::swap(*smallest_itr,*next);
      smallest_itr = next;
      next++;
    }
  }

  // BMW-Forwarding: Forwards beyond current block config
  void forward_lists_bmw(std::vector<plist_wrapper*>& postings_lists,
                const typename std::vector<plist_wrapper*>::iterator& 
                pivot_list, const uint64_t docid) {

    // Find the shortest list
    auto smallest_iter = find_shortest_list(postings_lists, pivot_list+1, docid);
    // Determine the next ID which might need to be evaluated
    auto list_end = postings_lists.end();
    auto iter = postings_lists.begin();
    auto end = pivot_list + 1;
    uint64_t candidate_id = std::numeric_limits<uint64_t>::max();

    // 'shallow' forwarding - a block-max array look-up
    while (iter != end) {
      // The last ID in the block [without needed to actually skip to it]
      uint64_t bid = (*iter)->cur.block_containing_id(docid);
      uint64_t block_candidate = (*iter)->cur.block_rep(bid) + 1;
      candidate_id = std::min(candidate_id, block_candidate);
      ++iter;
    }
    // If the pivot was not in the last list, we must also consider the
    // smallest DocID from the other remaining lists. Skipping this step
    // will result in loss of safe-to-k results.
    if (end != list_end) {
      candidate_id = std::min(candidate_id, (*end)->cur.docid());
    }

    // Corner case check
    if (candidate_id < docid)
      candidate_id = docid + 1;
   
    // Advance the smallest list to our new candidate
    (*smallest_iter)->cur.skip_to_id(candidate_id);
    
    // If the smallest list is finished, reorder the lists
    if ((*smallest_iter)->cur == (*smallest_iter)->end) {
      sort_list_by_id(postings_lists);
      return;
    }

    // Bubble it down.
    auto next = smallest_iter + 1;
    while (next != list_end && 
          (*smallest_iter)->cur.docid() > (*next)->cur.docid()) {
      std::swap(*smallest_iter, *next);
      smallest_iter = next;
      ++next;
    }
  }

  // Block-Max specific candidate test. Tests that the current pivot's block-max
  // scores still exceed the heap threshold. Returns the block-max score sum and
  // a boolean (whether we should indeed score, or not)
  const std::pair<bool, double> 
  potential_candidate(std::vector<plist_wrapper*>& postings_lists,
                      const typename std::vector<plist_wrapper*>::iterator& 
                      pivot_list, const double threshold, 
                      const uint64_t doc_id){

    auto iter = postings_lists.begin();
    double block_max_score = (*pivot_list)->cur.block_max(); // pivot blockmax

    // Lists preceding pivot list block max scores
    while (iter != pivot_list) {
      uint64_t bid = (*iter)->cur.block_containing_id(doc_id);
      block_max_score += (*iter)->cur.block_max(bid);
      ++iter;
    }

    // block-max test
    if (block_max_score > threshold) {
      return {true,block_max_score};
    }
    return {false,block_max_score};
  }

  // Returns a pivot document and its candidate (UB estimated) score.
  // Conjunctive pivot selection, can be used by BMW and Wand algos
  std::pair<typename std::vector<plist_wrapper*>::iterator, double>
  determine_candidate(std::vector<plist_wrapper*>& postings_lists) {
    // Return the doc in the last list since it's furtherest along (and
    // the only doc that may contain ALL terms). Also return our
    // pre-computed sum of all UB scores (was computed upon recieving query).
    return {postings_lists.end() - 1, m_conjunctive_max};
  }

  // Returns a pivot document and its candidate (UB estimated) score.
  // For disjunctive processing, can be used by BMW and Wand algos.
  std::pair<typename std::vector<plist_wrapper*>::iterator, double>
  determine_candidate(std::vector<plist_wrapper*>& postings_lists,
                      double threshold) {

    threshold = threshold * m_F; //Theta push
    double score = 0;
    auto itr = postings_lists.begin();
    auto end = postings_lists.end();
    while(itr != end) {
      score += (*itr)->list_max_score;
      if(score > threshold) {
        // forward to last list equal to pivot
        auto pivot_id = (*itr)->cur.docid();
        auto next = itr+1;
        while(next != end && (*next)->cur.docid() == pivot_id) {
          itr = next;
          score += (*itr)->list_max_score;
          ++next;
        }
        #ifdef PROFILE
          unique_pivots.insert(pivot_id);
        #endif
        return {itr,score};
      }
      ++itr;
    }
    return {end,score};
  }

  // Evaluates the pivot document
  double evaluate_pivot(std::vector<plist_wrapper*>& postings_lists,
                        std::priority_queue<doc_score,
                        std::vector<doc_score>,
                        std::greater<doc_score>>& heap,
                        double potential_score,
                        const double threshold,
                        const size_t k) {

    auto doc_id = postings_lists[0]->cur.docid(); //Pivot ID
    double doc_score = 0;
    double W_d = ranker->doc_length(doc_id);
    auto itr = postings_lists.begin();
    auto end = postings_lists.end();
    // Iterate postings 
    while (itr != end) {
      // Score the document if
      if ((*itr)->cur.docid() == doc_id) {
        #ifdef PROFILE
          ++postings_evaluated;
        #endif  
        double contrib = ranker->calculate_docscore((*itr)->cur.freq(),
                                                   (*itr)->f_t,
                                                   W_d);
        doc_score += contrib;
        potential_score += contrib;
        potential_score -= (*itr)->list_max_score; //Incremental refinement
        ++((*itr)->cur); // move to next larger doc_id
        // Check the refined potential max score 
        if (potential_score < threshold) {
          // Doc can no longer make the heap. Forward relevant lists.
          itr++;
          while (itr != end && (*itr)->cur != (*itr)->end 
                            && (*itr)->cur.docid() == doc_id) {
            ++((*itr)->cur);
            itr++;
          }
          break;
        }
      } 
      else {
        #ifdef PROFILE
          ++docs_fully_evaluated;
        #endif  
        break;
      }
      itr++;
    }
    // add if it is in the top-k
    if (heap.size() < k) {
      heap.push({doc_id,doc_score});
      #ifdef PROFILE
        ++docs_added_to_heap;
      #endif    
    } 
    else {
      if (heap.top().score < doc_score) {
        heap.pop();
        heap.push({doc_id,doc_score});
        #ifdef PROFILE
          ++docs_added_to_heap;
        #endif     
      }
    }
    
    #ifdef HORIZON
      //std::cerr << k <<  ",score," << doc_id << "," << doc_score << "\n";
      std::cerr << k << ",threshold," << doc_id << "," << heap.top().score << "\n";
    #endif
 
    // resort
    sort_list_by_id(postings_lists);
    if (heap.size() == k) {
      #ifdef PROFILE
        final_threshold = heap.top().score;
      #endif
      return heap.top().score;
    }
    return 0.0f;
  }


  // Block-Max pivot evaluation
  double evaluate_pivot_bmw(std::vector<plist_wrapper*>& postings_lists,
                        std::priority_queue<doc_score,
                        std::vector<doc_score>,
                        std::greater<doc_score>>& heap,
                        double potential_score,
                        const double threshold,
                        const size_t k) {

    uint64_t doc_id = postings_lists[0]->cur.docid(); // pivot
    double doc_score = 0;
    double W_d = ranker->doc_length(doc_id);
    auto itr = postings_lists.begin();
    auto end = postings_lists.end();
    
    // Iterate PLs
    while (itr != end) {
      // If we have the pivot, contribute the score
      if ((*itr)->cur.docid() == doc_id) {
        #ifdef PROFILE
          ++postings_evaluated;
        #endif  
        double contrib = ranker->calculate_docscore((*itr)->cur.freq(),
                                                   (*itr)->f_t,
                                                   W_d);
        doc_score += contrib;
        potential_score += contrib;
        // Differs from WAND version as we use BM scores for estimation
        uint64_t bid = (*itr)->cur.block_containing_id(doc_id);
        potential_score -= (*itr)->cur.block_max(bid);
        ++((*itr)->cur); // move to next larger doc_id
        // Doc cannot make heap, but we need to forward lists anyway 
        if (potential_score < threshold) {
          // move the other equal ones ahead still! 
          itr++;
          while (itr != end && (*itr)->cur != (*itr)->end 
                            && (*itr)->cur.docid() == doc_id) {
            ++((*itr)->cur);
            itr++;
          }
          break;
        }  
      } 
      else {
        #ifdef PROFILE
          ++docs_fully_evaluated;
        #endif  
        break;
      }
      itr++;
    }
    // add if it is in the top-k
    if (heap.size() < k) {
      heap.push({doc_id,doc_score});
      #ifdef PROFILE
        ++docs_added_to_heap;
      #endif     
    } 
    else {
      if (heap.top().score < doc_score) {
        heap.pop();
        heap.push({doc_id,doc_score});
        #ifdef PROFILE

          ++docs_added_to_heap;
        #endif     
      }
    }
       
    #ifdef HORIZON
      std::cerr << k << ",threshold," << doc_id << "," << heap.top().score << "\n";
    #endif
 
    // resort
    sort_list_by_id(postings_lists);
    if (heap.size() == k) {
      #ifdef PROFILE
        final_threshold = heap.top().score;
      #endif
      return heap.top().score;
    }
    return 0.0f;
  }

  // Given a list of postings which are negated and a doc_id, check if the
  // doc_id is contained in any of the negated lists (that is, the doc contains
  // negated terms).
  bool is_negated(std::vector<plist_wrapper*>& negated_lists, 
                  const uint64_t doc_id) {
    // Ensure the negated list is sorted such that the first list has the
    // smallest cursor, and so on.
    sort_list_by_id(negated_lists);
    auto itr = negated_lists.begin();
    auto end = negated_lists.end();

    // Iterate from smallest to largest ID until we find a cursor that is ahead
    // of the doc_id of interest. At that point, we can stop.
    while (itr != end && (*itr)->cur.docid() <= doc_id) {
      // Skip to the target. This is done blockwise to avoid decompression
      (*itr)->cur.skip_to_id(doc_id);
      // Check the ID
      if ((*itr)->cur != (*itr)->end && (*itr)->cur.docid() == doc_id) {
        #ifdef PROFILE
          ++negation_failed;
        #endif  
        return true; // This doc contains a negated term
      }
      ++itr; // Keep looking
    }
    #ifdef PROFILE
      ++negation_passed;
    #endif
    return false; // This document is OK
  }
 
  // Wand Disjunctive Algorithm
  result process_wand_disjunctive(std::vector<plist_wrapper*>& postings_lists,
                                  const size_t k) {
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;

    // init list processing 
    double threshold = 0.0f;

    // Initial Sort, get the pivot and its potential score
    sort_list_by_id(postings_lists);
    auto pivot_and_score = determine_candidate(postings_lists, threshold);
    auto pivot_list = std::get<0>(pivot_and_score);
    auto potential_score = std::get<1>(pivot_and_score);

    // While our pivot doc is not the end of the PL
    while (pivot_list != postings_lists.end()) {
      // If the first posting ID is that of the pivot, evaluate!
      if (postings_lists[0]->cur.docid() == (*pivot_list)->cur.docid()) {
          threshold = evaluate_pivot(postings_lists,
                                     score_heap,
                                     potential_score,
                                     threshold,
                                     k);
      }
      // We must forward the lists before the puvot up to our pivot doc  
      else {
        forward_lists(postings_lists,pivot_list,(*pivot_list)->cur.docid());
      }
      // Grsb the next pivot and its potential score
      pivot_and_score = determine_candidate(postings_lists, threshold);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);

    }

    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

  // Wand Disjunctive Algorithm with negation included
  result process_wand_disjunctive(std::vector<plist_wrapper*>& postings_lists,
                                  std::vector<plist_wrapper*>& negated_lists,
                                  const size_t k) {

    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;

    // init list processing 
    double threshold = 0.0f;

    // Initial Sort, get the pivot and its potential score
    sort_list_by_id(postings_lists);
    sort_list_by_id(negated_lists);
    auto pivot_and_score = determine_candidate(postings_lists, threshold);
    auto pivot_list = std::get<0>(pivot_and_score);
    auto potential_score = std::get<1>(pivot_and_score);

    // While our pivot doc is not the end of the PL
    while (pivot_list != postings_lists.end()) {

      // Now that we have a pivot that /might/ make the top-k, we need to
      // make sure it is negated before scoring it
      auto pivot_doc = (*pivot_list)->cur.docid();
      bool negated = is_negated(negated_lists, pivot_doc);

      // If the first posting ID is that of the pivot, evaluate!
      if (postings_lists[0]->cur.docid() ==  pivot_doc && !negated) {
          threshold = evaluate_pivot(postings_lists,
                                     score_heap,
                                     potential_score,
                                     threshold,
                                     k);
      }
      // Since this document is no longer worth considering, we skip forward
      else if (negated) {
        forward_lists(postings_lists, pivot_list, ++pivot_doc);
      }
      // We must forward the lists before the puvot up to our pivot doc  
      else {
        forward_lists(postings_lists,pivot_list,(*pivot_list)->cur.docid());
      }
      // Grab the next pivot and its potential score
      pivot_and_score = determine_candidate(postings_lists, threshold);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);

    }

    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }
 
  // Wand Conjunctive Algorithm
  result process_wand_conjunctive(std::vector<plist_wrapper*>& postings_lists,
                                  const size_t k) {
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;

    // init list processing 
    double threshold = 0.0f;
    // Initial Sort, get the pivot and its potential score
    sort_list_by_id(postings_lists);
    size_t initial = postings_lists.size();
    auto pivot_and_score = determine_candidate(postings_lists);
    auto pivot_list = std::get<0>(pivot_and_score);
    auto potential_score = std::get<1>(pivot_and_score);

    // While our pivot doc is not the end of the PL and we have not exhausted
    // any of our PL's
    while (pivot_list != postings_lists.end() && 
                postings_lists.size() == initial) {
      // If the first posting ID is that of the pivot, evaluate!
      if (postings_lists[0]->cur.docid() == (*pivot_list)->cur.docid()) {
          threshold = evaluate_pivot(postings_lists,
                                     score_heap,
                                     potential_score,
                                     threshold,
                                     k);
      }
      // We must forward the lists before the pivot up to our pivot doc  
      else {
        forward_lists(postings_lists,pivot_list,(*pivot_list)->cur.docid());
      }
      // Grsb the next pivot and its potential score
      pivot_and_score = determine_candidate(postings_lists);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);

    }

    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

  // BlockMax Wand Disjunctive
  result process_bmw_disjunctive(std::vector<plist_wrapper*>& postings_lists,
                            const size_t k){   
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;
    
    // init list processing , grab first pivot and potential score
    double threshold = 0;
    sort_list_by_id(postings_lists);
    auto pivot_and_score = determine_candidate(postings_lists, threshold);
    auto pivot_list = std::get<0>(pivot_and_score);

    // While we have got documents left to evaluate
    while (pivot_list != postings_lists.end()) {
      uint64_t candidate_id = (*pivot_list)->cur.docid();
      // Second level candidate check
      auto candidate_and_score = potential_candidate(postings_lists, pivot_list,
                                          threshold, candidate_id);
      auto candidate = std::get<0>(candidate_and_score);
      auto potential_score = std::get<1>(candidate_and_score);
      // If the document is still a candidate from BM scores
      if (candidate) {
        // If lists are aligned for pivot, score the doc
        if (postings_lists[0]->cur.docid() == candidate_id) {
          threshold = evaluate_pivot_bmw(postings_lists, score_heap,
                                     potential_score, threshold, k);
        }
        // Need to forward list before the pivot 
        else {
          forward_lists(postings_lists,pivot_list,candidate_id);
        }
      }
      // Use the knowledge that current block-max config can not yield a
      // solution, and skip to the next possible fruitful configuration
      else {
        forward_lists_bmw(postings_lists,pivot_list,candidate_id); 
      }
      // Grab a new pivot and keep going!
      pivot_and_score = determine_candidate(postings_lists,
                                            threshold);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);
      
    }

    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

  // BlockMax Wand Disjunctive with negation included (bm check first)
  result process_bmw_disjunctive_v1(std::vector<plist_wrapper*>& postings_lists,
                                 std::vector<plist_wrapper*>& negated_lists,
                                 const size_t k){   
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;
    
    // init list processing , grab first pivot and potential score
    double threshold = 0;
    sort_list_by_id(postings_lists);
    sort_list_by_id(negated_lists);
    auto pivot_and_score = determine_candidate(postings_lists, threshold);
    auto pivot_list = std::get<0>(pivot_and_score);

    // While we have got documents left to evaluate
    while (pivot_list != postings_lists.end()) {
      uint64_t candidate_id = (*pivot_list)->cur.docid();
      
      // Second level candidate check
      auto candidate_and_score = potential_candidate(postings_lists, pivot_list,
                                          threshold, candidate_id);
      auto candidate = std::get<0>(candidate_and_score);
      auto potential_score = std::get<1>(candidate_and_score);
      // If the document is still a candidate from BM scores
      if (candidate) {

        // V1: NOW we check the negation (after the BM check)
        bool negated = is_negated(negated_lists, candidate_id);

        // If lists are aligned for pivot, score the doc
        if (postings_lists[0]->cur.docid() == candidate_id && !negated) {
          threshold = evaluate_pivot_bmw(postings_lists, score_heap,
                                     potential_score, threshold, k);
        }
        // This doc contains negated terms, so we skip ahead
        else if (negated) {
          forward_lists(postings_lists, pivot_list, ++candidate_id);
        }
        // Need to forward list before the pivot 
        else {
          forward_lists(postings_lists,pivot_list,candidate_id);
        }
      }
      // Use the knowledge that current block-max config can not yield a
      // solution, and skip to the next possible fruitful configuration
      else {
        forward_lists_bmw(postings_lists,pivot_list,candidate_id); 
      }
      // Grab a new pivot and keep going!
      pivot_and_score = determine_candidate(postings_lists,
                                            threshold);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);
      
    }
    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

   // BlockMax Wand Disjunctive with negation included (negation check first)
  result process_bmw_disjunctive_v2(std::vector<plist_wrapper*>& postings_lists,
                                 std::vector<plist_wrapper*>& negated_lists,
                                 const size_t k){   
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;
    
    // init list processing , grab first pivot and potential score
    double threshold = 0;
    sort_list_by_id(postings_lists);
    sort_list_by_id(negated_lists);
    auto pivot_and_score = determine_candidate(postings_lists, threshold);
    auto pivot_list = std::get<0>(pivot_and_score);

    // While we have got documents left to evaluate
    while (pivot_list != postings_lists.end()) {
      uint64_t candidate_id = (*pivot_list)->cur.docid();

      // V2: We check for negation before we check the BM score
      bool negated = is_negated(negated_lists, candidate_id);
      // This doc contains negated terms, so we skip ahead
      if (negated) {
        forward_lists(postings_lists, pivot_list, ++candidate_id);

        // Grab a new pivot and go from the top of the loop
        pivot_and_score = determine_candidate(postings_lists,
                                            threshold);
        pivot_list = std::get<0>(pivot_and_score);
        continue;
      }

      // Second level candidate check
      auto candidate_and_score = potential_candidate(postings_lists, pivot_list,
                                          threshold, candidate_id);
      auto candidate = std::get<0>(candidate_and_score);
      auto potential_score = std::get<1>(candidate_and_score);
      // If the document is still a candidate from BM scores
      if (candidate) {
        // If lists are aligned for pivot, score the doc
        if (postings_lists[0]->cur.docid() == candidate_id) {
          threshold = evaluate_pivot_bmw(postings_lists, score_heap,
                                     potential_score, threshold, k);
        }
        // Need to forward list before the pivot 
        else {
          forward_lists(postings_lists,pivot_list,candidate_id);
        }
      }
      // Use the knowledge that current block-max config can not yield a
      // solution, and skip to the next possible fruitful configuration
      else {
        forward_lists_bmw(postings_lists,pivot_list,candidate_id); 
      }
      // Grab a new pivot and keep going!
      pivot_and_score = determine_candidate(postings_lists,
                                            threshold);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);
      
    }
    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

  // BlockMax Wand Conjunctive
  result process_bmw_conjunctive(std::vector<plist_wrapper*>& postings_lists,
                                const size_t k){   
    result res;
    // heap containing the top-k docs
    std::priority_queue<doc_score,std::vector<doc_score>,
                        std::greater<doc_score>> score_heap;
    
    // init list processing , grab first pivot and potential score
    double threshold = 0;
    sort_list_by_id(postings_lists);
    auto pivot_and_score = determine_candidate(postings_lists);
    auto pivot_list = std::get<0>(pivot_and_score);
    size_t initial = postings_lists.size();

    // While we have got documents left to evaluate
    while (pivot_list != postings_lists.end() &&
                         postings_lists.size() == initial) {
      uint64_t candidate_id = (*pivot_list)->cur.docid();
      // Second level candidate check
      auto candidate_and_score = potential_candidate(postings_lists, pivot_list,
                                          threshold, candidate_id);
      auto candidate = std::get<0>(candidate_and_score);
      auto potential_score = std::get<1>(candidate_and_score);
      // If the document is still a candidate from BM scores
      if (candidate) {
        // If lists are aligned for pivot, score the doc
        if (postings_lists[0]->cur.docid() == candidate_id) {
          threshold = evaluate_pivot_bmw(postings_lists, score_heap,
                                     potential_score, threshold, k);
        }
        // Need to forward list before the pivot 
        else {
          forward_lists(postings_lists,pivot_list,candidate_id);
        }
      }
      // Use the knowledge that current block-max config can not yield a
      // solution, and skip to the next possible fruitful configuration
      else {
        forward_lists_bmw(postings_lists,pivot_list,candidate_id); 
      }
      // Grab a new pivot and keep going!
      pivot_and_score = determine_candidate(postings_lists);
      pivot_list = std::get<0>(pivot_and_score);
      potential_score = std::get<1>(pivot_and_score);
      
    }

    // return the top-k results
    res.list.resize(score_heap.size());
    for (size_t i=0;i<res.list.size();i++) {
      auto min = score_heap.top(); score_heap.pop();
      res.list[res.list.size()-1-i] = min;
    }
    return res;
  }

  // Basic tool for dumping the union of the negated and disjunction terms
  // for a given query.
  void union_count(const std::vector<query_token>& qry, const size_t id) {
    std::unordered_set<uint64_t> negated;
    std::unordered_set<uint64_t> disjunctive;

    std::vector<plist_wrapper> pl_data(qry.size());
    std::vector<plist_wrapper> negated_data(qry.size());

    size_t j=0,n=0;
    for (const auto& qry_token : qry) {
      if (qry_token.negated) {
        negated_data[n] =  plist_wrapper(m_postings_lists[qry_token.token_id]);
        ++n;
      }
      else {
        pl_data[j] = plist_wrapper(m_postings_lists[qry_token.token_id]);
        ++j;
      }
    }

    negated_data.resize(n);
    pl_data.resize(j);

    // Run through negated lists
    for (size_t i = 0; i < negated_data.size(); ++i) {
      auto& pl = negated_data[i];
      while (pl.cur != pl.end) {
        auto doc_id = pl.cur.docid();
        negated.insert(doc_id);
        ++(pl.cur);
      } 
    }
    // Run through disjunctive lists
    for (size_t i = 0; i < pl_data.size(); ++i) {
      auto& pl = pl_data[i];
      while (pl.cur != pl.end) {
        auto doc_id = pl.cur.docid();
        disjunctive.insert(doc_id);
        ++(pl.cur);
      } 
    }

    // Calculate the intersection of these lists
    size_t intersection = 0;
    for (auto it = negated.begin(); it != negated.end(); ++it) {
      if(disjunctive.find(*it) != disjunctive.end())
        ++intersection;
    }

    std::cerr << id << "," << disjunctive.size()
                    << "," << negated.size()
                    << "," << intersection << std::endl;
    
  }

  result search(const std::vector<query_token>& qry, const size_t k,
                const index_form t_index_type,
                const query_traversal t_index_traversal,
                bool version_two = false) {

    #ifdef PROFILE
    postings_evaluated = 0;
    docs_fully_evaluated = 0;
    docs_added_to_heap = 0; 
    final_threshold = 0; 
    negation_passed = 0;
    negation_failed = 0;
    unique_pivots.clear();
    #endif

    m_conjunctive_max = 0.0f; // Reset for new query

    std::vector<plist_wrapper> pl_data(qry.size());
    std::vector<plist_wrapper*> postings_lists;
    std::vector<plist_wrapper> negated_data(qry.size());
    std::vector<plist_wrapper*> negated_lists;

    size_t j=0,n=0;
    for (const auto& qry_token : qry) {
      if (qry_token.negated) {
        negated_data[n] =  plist_wrapper(m_postings_lists[qry_token.token_id]);
        negated_lists.emplace_back(&(negated_data[n]));
        ++n;
      }
      else {
        pl_data[j] = plist_wrapper(m_postings_lists[qry_token.token_id]);
        postings_lists.emplace_back(&(pl_data[j]));
        m_conjunctive_max += pl_data[j].list_max_score;
        ++j;
      }
    }

    negated_data.resize(n);
    pl_data.resize(j);

    result res;

    // Select and run query
    if (t_index_type == BMW) {
      if (t_index_traversal == OR && n == 0)
        res = process_bmw_disjunctive(postings_lists,k);
      else if (t_index_traversal == OR && n > 0 && !version_two)
        res = process_bmw_disjunctive_v1(postings_lists,negated_lists,k);
      else if (t_index_traversal == OR && n > 0 && version_two)
        res = process_bmw_disjunctive_v2(postings_lists,negated_lists,k);
      else if (t_index_traversal == AND)
        res = process_bmw_conjunctive(postings_lists,k);
    }


    else if (t_index_type == WAND) {
      if (t_index_traversal == OR && n == 0)
        res = process_wand_disjunctive(postings_lists,k);
      else if (t_index_traversal == OR && n > 0)
        res = process_wand_disjunctive(postings_lists,negated_lists,k);
      else if (t_index_traversal == AND)
        res = process_wand_conjunctive(postings_lists,k);
    }
    
    else {
      std::cerr << "Invalid run-type selected. Must be wand or bmw."
                << std::endl;
      exit(EXIT_FAILURE);
    }
    #ifdef PROFILE
    res.postings_evaluated = postings_evaluated;
    res.docs_fully_evaluated = docs_fully_evaluated;
    res.docs_added_to_heap = docs_added_to_heap; 
    res.final_threshold = final_threshold; 
    res.negation_passed = negation_passed;
    res.negation_failed = negation_failed;
    res.unique_pivots = unique_pivots.size();
    #endif
    return res; 
  }

};

// Search
template<class t_pl,class t_rank>
void construct(idx_invfile<t_pl,t_rank> &idx,
               std::string& postings_file, 
                const double F)
{
    using namespace sdsl;
    cout << "construct(idx_invfile)"<< endl;
    idx = idx_invfile<t_pl,t_rank>(postings_file, F);
    cout << "Done" << endl;
}
#endif

