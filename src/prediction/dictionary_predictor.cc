// Copyright 2010-2012, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "prediction/dictionary_predictor.h"

#include <limits.h>   // INT_MAX
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include "base/base.h"
#include "base/trie.h"
#include "base/util.h"
#include "composer/composer.h"
#include "config/config.pb.h"
#include "config/config_handler.h"
#include "converter/connector_interface.h"
#include "converter/conversion_request.h"
#include "converter/immutable_converter_interface.h"
#include "converter/node.h"
#include "converter/node_allocator.h"
#include "converter/segmenter_interface.h"
#include "converter/segments.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_matcher.h"
#include "prediction/predictor_interface.h"
#include "prediction/suggestion_filter.h"
#include "prediction/zero_query_number_data.h"
#include "session/commands.pb.h"
#include "session/request_handler.h"

// This flag is set by predictor.cc
// We can remove this after the ambiguity expansion feature get stable.
DEFINE_bool(enable_expansion_for_dictionary_predictor,
            false,
            "enable ambiguity expansion for dictionary_predictor");

namespace mozc {
namespace {

// Note that PREDICTION mode is much slower than SUGGESTION.
// Number of prediction calls should be minimized.
const size_t kSuggestionMaxNodesSize = 256;
const size_t kPredictionMaxNodesSize = 100000;

bool IsNumber(const string &str) {
  for (string::const_iterator it = str.begin(); it != str.end(); ++it) {
    if (!isdigit(*it)) {
      return false;
    }
  }
  return true;
}

void GetNumberSuffixArray(const string &history_input,
                          vector<string> *suffixes) {
  DCHECK(suffixes);
  const char kDefault[] = "default";
  const string default_str(kDefault);

  int default_num = -1;
  int suffix_num = -1;

  for (int i = 0; ZeroQueryNum[i]; ++i) {
    if (default_str == ZeroQueryNum[i][0]) {
      default_num = i;
    } else if (history_input == ZeroQueryNum[i][0]) {
      suffix_num = i;
    }
  }
  DCHECK_GE(default_num, 0);

  if (suffix_num != -1) {
    for (int j = 1; ZeroQueryNum[suffix_num][j]; ++j) {
      suffixes->push_back(ZeroQueryNum[suffix_num][j]);
    }
  }
  for (int j = 1; ZeroQueryNum[default_num][j]; ++j) {
    suffixes->push_back(ZeroQueryNum[default_num][j]);
  }
  DCHECK_GE(suffixes->size(), 0);
}
}  // namespace

DictionaryPredictor::DictionaryPredictor(
    const ImmutableConverterInterface *immutable_converter,
    const DictionaryInterface *dictionary,
    const DictionaryInterface *suffix_dictionary,
    const ConnectorInterface *connector,
    const SegmenterInterface *segmenter,
    const POSMatcher &pos_matcher)
    : immutable_converter_(immutable_converter),
      dictionary_(dictionary),
      suffix_dictionary_(suffix_dictionary),
      connector_(connector),
      segmenter_(segmenter),
      counter_suffix_word_id_(pos_matcher.GetCounterSuffixWordId()),
      predictor_name_("DictionaryPredictor") {}

DictionaryPredictor::~DictionaryPredictor() {}

bool DictionaryPredictor::Predict(Segments *segments) const {
  ConversionRequest default_request;
  return PredictForRequest(default_request, segments);
}

bool DictionaryPredictor::PredictForRequest(const ConversionRequest &request,
                                            Segments *segments) const {
  if (segments == NULL) {
    return false;
  }

  vector<Result> results;
  scoped_ptr<NodeAllocatorInterface> allocator(new NodeAllocator);

  if (!AggregatePrediction(request, segments, allocator.get(), &results)) {
    return false;
  }

  SetCost(*segments, &results);
  RemovePrediction(*segments, &results);
  return AddPredictionToCandidates(segments, &results);
}

bool DictionaryPredictor::AggregatePrediction(
    const ConversionRequest &request,
    Segments *segments, NodeAllocatorInterface *allocator,
    vector<Result> *results) const {
  DCHECK(segments);
  DCHECK(results);

  const PredictionType prediction_type = GetPredictionType(*segments);
  if (prediction_type == NO_PREDICTION) {
    return false;
  }

  if (segments->request_type() == Segments::PARTIAL_SUGGESTION ||
      segments->request_type() == Segments::PARTIAL_PREDICTION) {
      // This request type is used to get conversion before cursor during
    // composition mode. Thus it should return only the candidates whose key
    // exactly matches the query.
    // Therefore, we use only the realtime conversion result.
    AggregateRealtimeConversion(prediction_type, segments, allocator, results);
  } else {
    AggregateRealtimeConversion(prediction_type, segments, allocator, results);
    AggregateUnigramPrediction(prediction_type, request, segments,
                               allocator, results);
    AggregateBigramPrediction(prediction_type, request, segments,
                              allocator, results);
    AggregateSuffixPrediction(prediction_type, request, segments,
                              allocator, results);
  }

  if (results->empty()) {
    VLOG(2) << "|result| is empty";
    return false;
  } else {
    return true;
  }
}

void DictionaryPredictor::SetCost(const Segments &segments,
                                  vector<Result> *results) const {
  DCHECK(results);

  if (GET_REQUEST(mixed_conversion)) {
    SetLMCost(segments, results);
  } else {
    SetPredictionCost(segments, results);
  }

  ApplyPenaltyForKeyExpansion(segments, results);
}

void DictionaryPredictor::RemovePrediction(const Segments &segments,
                                           vector<Result> *results) const {
  DCHECK(results);

  if (!GET_REQUEST(mixed_conversion)) {
    // Currently, we don't have spelling correction feature on mobile,
    // so we don't run RemoveMissSpelledCandidates.
    const string &input_key = segments.conversion_segment(0).key();
    const size_t input_key_len = Util::CharsLen(input_key);
    RemoveMissSpelledCandidates(input_key_len, results);
  }
}

bool DictionaryPredictor::AddPredictionToCandidates(
    Segments *segments, vector<Result> *results) const {
  DCHECK(segments);
  DCHECK(results);
  const bool mixed_conversion = GET_REQUEST(mixed_conversion);

  const string &input_key = segments->conversion_segment(0).key();
  const size_t input_key_len = Util::CharsLen(input_key);

  string history_key, history_value;
  GetHistoryKeyAndValue(*segments, &history_key, &history_value);

  // exact_bigram_key does not contain ambiguity expansion, because
  // this is used for exact matching for the key.
  const string exact_bigram_key = history_key + input_key;

  Segment *segment = segments->mutable_conversion_segment(0);
  DCHECK(segment);

  // Instead of sorting all the results, we construct a heap.
  // This is done in linear time and
  // we can pop as many results as we need efficiently.
  make_heap(results->begin(), results->end(), ResultCompare());

  const size_t size = min(segments->max_prediction_candidates_size(),
                          results->size());

  int added = 0;
  set<string> seen;

  for (size_t i = 0; i < results->size(); ++i) {
    if (added >= size || results->at(i).cost == INT_MAX) {
      break;
    }

    pop_heap(results->begin(), results->end() - i, ResultCompare());
    const Result &result = results->at(results->size() - i - 1);
    const Node *node = result.node;
    DCHECK(node);

    if (result.type == NO_PREDICTION) {
      continue;
    }

    // We don't filter the results from realtime conversion if mixed_conversion
    // is true.
    // TODO(manabe): Add a unit test. For that, we'll need a mock class for
    //               SuppressionDictionary.
    if (SuggestionFilter::IsBadSuggestion(node->value) &&
        !(mixed_conversion && result.type & REALTIME)) {
      continue;
    }

    // don't suggest exactly the same candidate as key.
    // if |mixed_conversion| is true, that's not the case.
    if (!mixed_conversion &&
        !(result.type & REALTIME) &&
        (((result.type & BIGRAM) &&
          exact_bigram_key == node->value) ||
         (!(result.type & BIGRAM) &&
          input_key == node->value))) {
      continue;
    }

    string key, value;
    if (result.type & BIGRAM) {
      // remove the prefix of history key and history value.
      key = node->key.substr(history_key.size(),
                             node->key.size() - history_key.size());
      value = node->value.substr(history_value.size(),
                                 node->value.size() - history_value.size());
    } else {
      key = node->key;
      value = node->value;
    }

    if (!seen.insert(value).second) {
      continue;
    }

    // User input: "おーすとり" (len = 5)
    // key/value:  "おーすとりら" "オーストラリア" (miss match pos = 4)
    if ((node->attributes & Node::SPELLING_CORRECTION) &&
        key != input_key &&
        input_key_len <= GetMissSpelledPosition(key, value) + 1) {
      continue;
    }

    Segment::Candidate *candidate = segment->push_back_candidate();
    DCHECK(candidate);

    candidate->Init();
    candidate->content_key = key;
    candidate->content_value = value;
    candidate->key = key;
    candidate->value = value;
    candidate->lid = node->lid;
    candidate->rid = node->rid;
    candidate->wcost = node->wcost;
    candidate->cost = results->at(i).cost;
    if (node->attributes & Node::SPELLING_CORRECTION) {
      candidate->attributes |= Segment::Candidate::SPELLING_CORRECTION;
    }

    // Don't provide any descriptions for dictionary suggests
#ifdef _DEBUG
    const char kRealtimeConversionDescription[] = "Real-time Conversion";
    const char kDictionarySuggestDescription[] = "Dictionary Suggest";
    if (result.type & REALTIME) {
      candidate->description = kRealtimeConversionDescription;
    } else {
      candidate->description = kDictionarySuggestDescription;
    }
#endif
    ++added;
  }

  return added > 0;
}

// return transition_cost[rid][node.lid] + node.wcost (+ penalties).
int DictionaryPredictor::GetLMCost(PredictionType type,
                                   const Node &node,
                                   int rid) const {
  int lm_cost = connector_->GetTransitionCost(rid, node.lid) + node.wcost;
  if (!(type & REALTIME)) {
    // Relatime conversion already adds perfix/suffix penalties to the nodes.
    // Note that we don't add prefix penalty the role of "bunsetsu" is
    // ambigous on zero-query suggestion.
    lm_cost += segmenter_->GetSuffixPenalty(node.rid);
  }

  return lm_cost;
}

// return dictionary node whose value/key are |key| and |value|.
// return NULL no words are found in the dictionary.
const Node *DictionaryPredictor::LookupKeyValueFromDictionary(
    const string &key,
    const string &value,
    NodeAllocatorInterface *allocator) const {
  DCHECK(allocator);
  const Node *node = dictionary_->LookupPrefix(key.data(), key.size(),
                                               allocator);
  for (; node != NULL; node = node->bnext) {
    if (value == node->value) {
      return node;
    }
  }
  return NULL;
}

bool DictionaryPredictor::GetHistoryKeyAndValue(
    const Segments &segments,
    string *key, string *value) const {
  DCHECK(key);
  DCHECK(value);
  if (segments.history_segments_size() > 0) {
    const Segment &history_segment =
        segments.history_segment(segments.history_segments_size() - 1);
    if (history_segment.candidates_size() > 0) {
      key->assign(history_segment.candidate(0).key);
      value->assign(history_segment.candidate(0).value);
      return true;
    }
  }
  return false;
}

void DictionaryPredictor::SetPredictionCost(const Segments &segments,
                                            vector<Result> *results) const {
  int rid = 0;  // 0 (BOS) is default
  if (segments.history_segments_size() > 0) {
    const Segment &history_segment =
        segments.history_segment(segments.history_segments_size() - 1);
    if (history_segment.candidates_size() > 0) {
      rid = history_segment.candidate(0).rid;  // use history segment's id
    }
  }

  DCHECK(results);
  const string &input_key = segments.conversion_segment(0).key();
  string history_key, history_value;
  GetHistoryKeyAndValue(segments, &history_key, &history_value);
  const string bigram_key = history_key + input_key;
  const bool is_suggestion = (segments.request_type() ==
                              Segments::SUGGESTION);

  // use the same scoring function for both unigram/bigram.
  // Bigram will be boosted because we pass the previous
  // key as a context information.
  const size_t bigram_key_len = Util::CharsLen(bigram_key);
  const size_t unigram_key_len = Util::CharsLen(input_key);

  for (size_t i = 0; i < results->size(); ++i) {
    const Node *node = (*results)[i].node;
    const PredictionType type = (*results)[i].type;
    const int32 cost = GetLMCost(type, *node, rid);
    DCHECK(node);

    const size_t query_len =
        ((*results)[i].type & BIGRAM) ? bigram_key_len : unigram_key_len;
    const size_t key_len = Util::CharsLen(node->key);

    if (IsAggressiveSuggestion(query_len, key_len, cost,
                               is_suggestion, results->size())) {
      (*results)[i].cost = INT_MAX;
      continue;
    }

    // cost = -500 * log(lang_prob(w) * (1 + remain_length))    -- (1)
    // where lang_prob(w) is a language model probability of the word "w", and
    // remain_length the length of key user must type to input "w".
    //
    // Example:
    // key/value = "とうきょう/東京"
    // user_input = "とう"
    // remain_length = len("とうきょう") - len("とう") = 3
    //
    // By taking the log of (1),
    // cost  = -500 [log(lang_prob(w)) + log(1 + ramain_length)]
    //       = -500 * log(lang_prob(w)) + 500 * log(1 + remain_length)
    //       = cost - 500 * log(1 + remain_length)
    // Because 500 * log(lang_prob(w)) = -cost.
    //
    // lang_prob(w) * (1 + remain_length) represents how user can reduce
    // the total types by choosing this candidate.
    // Before this simple algorithm, we have been using an SVM-base scoring,
    // but we stop usign it with the following reasons.
    // 1) Hard to maintain the ranking.
    // 2) Hard to control the final results of SVM.
    // 3) Hard to debug.
    // 4) Since we used the log(remain_length) as a feature,
    //    the new ranking algorithm and SVM algorithm was essentially
    //    the same.
    // 5) Since we used the length of value as a feature, we find
    //    inconsistencies between the conversion and the prediction
    //    -- the results of top prediction and the top conversion
    //    (the candidate shown after the space key) may differ.
    //
    // The new function brings consistent results. If two candidate
    // have the same reading (key), they should have the same cost bonus
    // from the length part. This implies that the result is reranked by
    // the language model probability as long as the key part is the same.
    // This behavior is baisically the same as the converter.
    //
    // TODO(team): want find the best parameter instread of kCostFactor.
    const int kCostFactor = 500;
    (*results)[i].cost = cost -
        kCostFactor * log(1.0 + max(0, static_cast<int>(key_len - query_len)));
  }
}

void DictionaryPredictor::SetLMCost(const Segments &segments,
                                    vector<Result> *results) const {
  DCHECK(results);

  // ranking for mobile
  int rid = 0;  // 0 (BOS) is default
  int prev_cost = 0;
  if (segments.history_segments_size() > 0) {
    const Segment &history_segment =
        segments.history_segment(segments.history_segments_size() - 1);
    if (history_segment.candidates_size() > 0) {
      rid = history_segment.candidate(0).rid;  // use history segment's id
      prev_cost = history_segment.candidate(0).cost;
      if (prev_cost == 0) {
        // if prev_cost is set to be 0 for some reason, use default cost.
        prev_cost = 5000;
      }
    }
  }

  for (size_t i = 0; i < results->size(); ++i) {
    const Node *node = (*results)[i].node;
    const PredictionType type = (*results)[i].type;
    DCHECK(node);
    int cost = GetLMCost(type, *node, rid);
    // Make exact candidates to have higher ranking.
    // Because for mobile, suggestion is the main candidates and
    // users expect the candidates for the input key on the candidates.
    if (type & UNIGRAM) {
      const size_t input_key_len = Util::CharsLen(
          segments.conversion_segment(0).key());
      const size_t key_len = Util::CharsLen(node->key);
      if (key_len > input_key_len) {
        // Cost penalty means that exact candiates are evaluated
        // 50 times bigger in frequency.
        // Note that the cost is calculated by cost = -500 * log(prob)
        // 1956 = 500 * log(50)
        const int kNotExactPenalty = 1956;
        cost += kNotExactPenalty;
      }
    }
    if (type & BIGRAM) {
      // When user inputs "六本木" and there is an entry
      // "六本木ヒルズ" in the dictionary, we can suggest
      // "ヒルズ" as a ZeroQuery suggestion. In this case,
      // We can't calcurate the transition cost between "六本木"
      // and "ヒルズ". If we ignore the transition cost,
      // bigram-based suggestion will be overestimated.
      // Here we use |default_transition_cost| as an
      // transition cost between "六本木" and "ヒルズ". Currently,
      // the cost is basically the same as the cost between
      // "名詞,一般" and "名詞,一般".
      const int kDefaultTransitionCost = 1347;
      cost += (kDefaultTransitionCost - prev_cost);
    }
    (*results)[i].cost = cost;
  }
}

void DictionaryPredictor::ApplyPenaltyForKeyExpansion(
    const Segments &segments, vector<Result> *results) const {
  if (segments.conversion_segments_size() == 0) {
    return;
  }
  // Cost penalty 1151 means that expanded candiates are evaluated
  // 10 times smaller in frequency.
  // Note that the cost is calcurated by cost = -500 * log(prob)
  // 1151 = 500 * log(10)
  const int kKeyExpansionPenalty = 1151;
  const string &conversion_key = segments.conversion_segment(0).key();
  for (size_t i = 0; i < results->size(); ++i) {
    const Node *node = (*results)[i].node;
    if (!Util::StartsWith(node->key, conversion_key)) {
      (*results)[i].cost += kKeyExpansionPenalty;
    }
  }
}

size_t DictionaryPredictor::GetMissSpelledPosition(
    const string &key, const string &value) const {
  string hiragana_value;
  Util::KatakanaToHiragana(value, &hiragana_value);
  // value is mixed type. return true if key == request_key.
  if (Util::GetScriptType(hiragana_value) != Util::HIRAGANA) {
    return Util::CharsLen(key);
  }

  // Find the first position of character where miss spell occurs.
  int position = 0;
  ConstChar32Iterator key_iter(key);
  for (ConstChar32Iterator hiragana_iter(hiragana_value);
       !hiragana_iter.Done() && !key_iter.Done();
       hiragana_iter.Next(), key_iter.Next(), ++position) {
    if (hiragana_iter.Get() != key_iter.Get()) {
      return position;
    }
  }

  // not find. return the length of key.
  while (!key_iter.Done()) {
    ++position;
    key_iter.Next();
  }

  return position;
}

void DictionaryPredictor::RemoveMissSpelledCandidates(
    size_t request_key_len,
    vector<Result> *results) const {
  DCHECK(results);

  if (results->size() <= 1) {
    return;
  }

  int spelling_correction_size = 5;
  for (size_t i = 0; i < results->size(); ++i) {
    const Result &result = (*results)[i];
    DCHECK(result.node);
    if (!(result.node->attributes & Node::SPELLING_CORRECTION)) {
      continue;
    }

    // Only checks at most 5 spelling corrections to avoid the case
    // like all candidates have SPELLING_CORRECTION.
    if (--spelling_correction_size == 0) {
      return;
    }

    vector<size_t> same_key_index, same_value_index;
    for (size_t j = 0; j < results->size(); ++j) {
      if (i == j) {
        continue;
      }
      const Result &target_result = (*results)[j];
      if (target_result.node->attributes & Node::SPELLING_CORRECTION) {
        continue;
      }
      if (target_result.node->key == result.node->key) {
        same_key_index.push_back(j);
      }
      if (target_result.node->value == result.node->value) {
        same_value_index.push_back(j);
      }
    }

    // delete same_key_index and same_value_index
    if (!same_key_index.empty() && !same_value_index.empty()) {
      (*results)[i].type = NO_PREDICTION;
      for (size_t k = 0; k < same_key_index.size(); ++k) {
        (*results)[same_key_index[k]].type = NO_PREDICTION;
      }
    } else if (same_key_index.empty() && !same_value_index.empty()) {
      (*results)[i].type = NO_PREDICTION;
    } else if (!same_key_index.empty() && same_value_index.empty()) {
      for (size_t k = 0; k < same_key_index.size(); ++k) {
        (*results)[same_key_index[k]].type = NO_PREDICTION;
      }
      if (request_key_len <=
          GetMissSpelledPosition(result.node->key,
                                 result.node->value)) {
        (*results)[i].type = NO_PREDICTION;
      }
    }
  }
}

bool DictionaryPredictor::IsAggressiveSuggestion(
    size_t query_len, size_t key_len, int32 cost,
    bool is_suggestion, size_t total_candidates_size) const {
  // Temporal workaround for fixing the problem where longer sentence-like
  // suggestions are shown when user input is very short.
  // "ただしい" => "ただしいけめんにかぎる"
  // "それでもぼ" => "それでもぼくはやっていない".
  // If total_candidates_size is small enough, we don't perform
  // special filtering. e.g., "せんとち" has only two candidates, so
  // showing "千と千尋の神隠し" is OK.
  // Also, if the cost is too small (< 5000), we allow to display
  // long phrases. Examples include "よろしくおねがいします".
  if (is_suggestion && total_candidates_size >= 10 && key_len >= 8 &&
      cost >= 5000 && query_len <= static_cast<size_t>(0.4 * key_len)) {
    return true;
  }

  return false;
}

size_t DictionaryPredictor::GetRealtimeCandidateMaxSize(
    const Segments &segments, bool mixed_conversion, size_t max_size) const {
  const Segments::RequestType request_type = segments.request_type();
  DCHECK(request_type == Segments::PREDICTION ||
         request_type == Segments::SUGGESTION ||
         request_type == Segments::PARTIAL_PREDICTION ||
         request_type == Segments::PARTIAL_SUGGESTION);
  const int kFewResultThreshold = 8;
  size_t default_size = 6;
  if (segments.segments_size() > 0 &&
      Util::CharsLen(segments.segment(0).key()) >= kFewResultThreshold) {
    // We don't make so many realtime conversion prediction
    // even if we have enough margin, as it's expected less useful.
    max_size = min(max_size, static_cast<size_t>(8));
    default_size = 3;
  }
  size_t size = 0;
  switch (request_type) {
    case Segments::PREDICTION:
      size = mixed_conversion ? max_size - default_size : default_size;
      break;
    case Segments::SUGGESTION:
      // Fewer candidatats are needed basically.
      // But on mixed_conversion mode we should behave like as conversion mode.
      size = mixed_conversion ? default_size : 1;
      break;
    case Segments::PARTIAL_PREDICTION:
      // This is kind of prediction so richer result than PARTIAL_SUGGESTION
      // is needed.
      size = max_size;
      break;
    case Segments::PARTIAL_SUGGESTION:
      // PARTIAL_SUGGESTION works like as conversion mode so returning
      // some candidates is needed.
      size = default_size;
      break;
    default:
      size = 0;  // Never reach here
  }
  return min(max_size, size);
}

void DictionaryPredictor::AggregateRealtimeConversion(
    PredictionType type,
    Segments *segments,
    NodeAllocatorInterface *allocator,
    vector<Result> *results) const {
  if (!(type & REALTIME)) {
    return;
  }

  DCHECK(immutable_converter_);
  DCHECK(segments);
  DCHECK(results);
  DCHECK(allocator);

  Segment *segment = segments->mutable_conversion_segment(0);
  DCHECK(segment);
  DCHECK(!segment->key().empty());

  // preserve the previous max_prediction_candidates_size,
  // and candidates_size.
  const size_t prev_candidates_size = segment->candidates_size();
  const size_t prev_max_prediction_candidates_size =
      segments->max_prediction_candidates_size();

  // set how many candidates we want to obtain with
  // immutable converter.
  const bool mixed_conversion = GET_REQUEST(mixed_conversion);
  const size_t realtime_candidates_size = GetRealtimeCandidateMaxSize(
      *segments,
      mixed_conversion,
      prev_max_prediction_candidates_size - prev_candidates_size);

  segments->set_max_prediction_candidates_size(prev_candidates_size +
                                               realtime_candidates_size);

  if (immutable_converter_->Convert(segments) &&
      prev_candidates_size < segment->candidates_size()) {
    // A little tricky treatment:
    // Since ImmutableConverter::Converter creates a set of new candidates,
    // copy them into the array of Results.
    for (size_t i = prev_candidates_size;
         i < segment->candidates_size(); ++i) {
      const Segment::Candidate &candidate = segment->candidate(i);
      Node *node= allocator->NewNode();
      DCHECK(node);
      node->Init();
      node->lid = candidate.lid;
      node->rid = candidate.rid;
      node->wcost = candidate.wcost;
      node->key = candidate.key;
      node->value = candidate.value;
      if (candidate.attributes & Segment::Candidate::SPELLING_CORRECTION) {
        node->attributes |= Node::SPELLING_CORRECTION;
      }
      results->push_back(Result(node, REALTIME));
    }
    // remove candidates created by ImmutableConverter.
    segment->erase_candidates(prev_candidates_size,
                              segment->candidates_size() -
                              prev_candidates_size);
    // restore the max_prediction_candidates_size.
    segments->set_max_prediction_candidates_size(
        prev_max_prediction_candidates_size);
  } else {
    LOG(WARNING) << "Convert failed";
  }
}

size_t DictionaryPredictor::GetUnigramCandidateCutoffThreshold(
    const Segments &segments,
    bool mixed_conversion) const {
  DCHECK(segments.request_type() == Segments::PREDICTION ||
         segments.request_type() == Segments::SUGGESTION);
  if (mixed_conversion) {
    return kSuggestionMaxNodesSize;
  }
  if (segments.request_type() == Segments::PREDICTION) {
    // If PREDICTION, many candidates are needed than SUGGESTION.
    return kPredictionMaxNodesSize;
  }
  return kSuggestionMaxNodesSize;
}

void DictionaryPredictor::AggregateUnigramPrediction(
    PredictionType type,
    const ConversionRequest &request,
    Segments *segments,
    NodeAllocatorInterface *allocator,
    vector<Result> *results) const {
  if (!(type & UNIGRAM)) {
    return;
  }

  DCHECK(segments);
  DCHECK(results);
  DCHECK(dictionary_);
  DCHECK(allocator);
  DCHECK(!segments->conversion_segment(0).key().empty());

  const bool mixed_conversion = GET_REQUEST(mixed_conversion);
  const size_t cutoff_threshold = GetUnigramCandidateCutoffThreshold(
      *segments,
      mixed_conversion);
  allocator->set_max_nodes_size(cutoff_threshold);

  const size_t prev_results_size = results->size();

  // no history key
  const Node *unigram_node = GetPredictiveNodes(
      dictionary_, "", request, *segments, allocator);
  size_t unigram_results_size = 0;
  for (; unigram_node != NULL; unigram_node = unigram_node->bnext) {
    results->push_back(Result(unigram_node, UNIGRAM));
    ++unigram_results_size;
  }

  // if size reaches max_nodes_size (== cutoff_threshold).
  // we don't show the candidates, since disambiguation from
  // 256 candidates is hard. (It may exceed max_nodes_size, because this is
  // just a limit for each backend, so total number may be larger)
  if (unigram_results_size >= allocator->max_nodes_size()) {
    results->resize(prev_results_size);
  }
}

void DictionaryPredictor::AggregateBigramPrediction(
    PredictionType type,
    const ConversionRequest &request,
    Segments *segments,
    NodeAllocatorInterface *allocator,
    vector<Result> *results) const {
  if (!(type & BIGRAM)) {
    return;
  }

  DCHECK(segments);
  DCHECK(results);
  DCHECK(dictionary_);
  DCHECK(allocator);

  const string &input_key = segments->conversion_segment(0).key();
  const bool is_zero_query = input_key.empty();

  string history_key, history_value;
  GetHistoryKeyAndValue(*segments, &history_key, &history_value);

  // Check that history_key/history_value are in the dictionary.
  const Node *history_node = LookupKeyValueFromDictionary(
      history_key, history_value, allocator);

  // History value is not found in the dictionary.
  // User may create this the history candidate from T13N or segment
  // expand/shrinkg operations.
  if (history_node == NULL) {
    return;
  }

  const size_t max_nodes_size =
      (segments->request_type() == Segments::PREDICTION) ?
      kPredictionMaxNodesSize : kSuggestionMaxNodesSize;
  allocator->set_max_nodes_size(max_nodes_size);

  const size_t prev_results_size = results->size();

  const Node *bigram_node = GetPredictiveNodes(
      dictionary_, history_key, request, *segments, allocator);
  size_t bigram_results_size = 0;
  for (; bigram_node != NULL; bigram_node = bigram_node->bnext) {
    // filter out the output (value)'s prefix doesn't match to
    // the history value.
    if (Util::StartsWith(bigram_node->value, history_value)) {
      results->push_back(Result(bigram_node, BIGRAM));
      ++bigram_results_size;
    }
  }

  // if size reaches max_nodes_size,
  // we don't show the candidates, since disambiguation from
  // 256 candidates is hard. (It may exceed max_nodes_size, because this is
  // just a limit for each backend, so total number may be larger)
  if (bigram_results_size >= allocator->max_nodes_size()) {
    results->resize(prev_results_size);
    return;
  }

  // Obtain the character type of the last history value.
  const size_t history_value_size = Util::CharsLen(history_value);
  if (history_value_size == 0) {
    return;
  }

  const Util::ScriptType last_history_ctype =
      Util::GetScriptType(Util::SubString(history_value,
                                          history_value_size - 1, 1));

  // Filter out irrelevant bigrams. For example, we don't want to
  // suggest "リカ" from the history "アメ".
  for (size_t i = prev_results_size; i < results->size(); ++i) {
    const Node *node = (*results)[i].node;
    DCHECK(node);
    const string key = node->key.substr(history_key.size(),
                                        node->key.size() -
                                        history_key.size());
    const string value = node->value.substr(history_value.size(),
                                            node->value.size() -
                                            history_value.size());
    // Don't suggest 0-length key/value.
    if (key.empty() || value.empty()) {
      (*results)[i].type = NO_PREDICTION;
      continue;
    }

    // If freq("アメ") < freq("アメリカ"), we don't
    // need to suggest it. As "アメリカ" should already be
    // suggested when user type "アメ".
    // Note that wcost = -500 * log(prob).
    if (history_node->wcost > node->wcost) {
      (*results)[i].type = NO_PREDICTION;
      continue;
    }

    // If character type doesn't change, this boundary might NOT
    // be a word boundary. If character type is HIRAGANA,
    // we don't trust it. If Katakana, only trust iif the
    // entire key is reasonably long.
    const Util::ScriptType ctype =
        Util::GetScriptType(Util::SubString(value, 0, 1));
    if (ctype == last_history_ctype &&
        (ctype == Util::HIRAGANA ||
         (ctype == Util::KATAKANA && Util::CharsLen(node->key) <= 5))) {
      (*results)[i].type = NO_PREDICTION;
      continue;
    }

    // The suggested key/value pair must exist in the dictionary.
    // For example, we don't want to suggest "ターネット" from
    // the history "イン".
    // If character type is Kanji and the suggestion is not a
    // zero_query_suggestion, we relax this condition, as there are
    // many Kanji-compounds which may not in the dictionary. For example,
    // we want to suggest "霊長類研究所" from the history "京都大学".
    if (ctype == Util::KANJI && is_zero_query) {
      // Do not filter this.
      continue;
    }

    if (NULL == LookupKeyValueFromDictionary(key, value, allocator)) {
      (*results)[i].type = NO_PREDICTION;
      continue;
    }
  }
}

const Node *DictionaryPredictor::GetPredictiveNodes(
    const DictionaryInterface *dictionary,
    const string &history_key,
    const ConversionRequest &request,
    const Segments &segments,
    NodeAllocatorInterface *allocator) const {
  if (!request.has_composer() ||
      !FLAGS_enable_expansion_for_dictionary_predictor) {
    const string input_key = history_key + segments.conversion_segment(0).key();
    return dictionary->LookupPredictive(input_key.c_str(),
                                        input_key.size(),
                                        allocator);
  } else {
    // If we have ambiguity for the input, get expanded key.
    // Example1 roman input: for "あk", we will get |base|, "あ" and |expanded|,
    // "か", "き", etc
    // Example2 kana input: for "あか", we will get |base|, "あ" and |expanded|,
    // "か", and "が".
    string base;
    set<string> expanded;
    request.composer().GetQueriesForPrediction(&base, &expanded);
    const string input_key = history_key + base;
    DictionaryInterface::Limit limit;
    scoped_ptr<Trie<string> > trie(NULL);
    if (expanded.size() > 0) {
      trie.reset(new Trie<string>);
      for (set<string>::const_iterator itr = expanded.begin();
           itr != expanded.end(); ++itr) {
        trie->AddEntry(*itr, "");
      }
      limit.begin_with_trie = trie.get();
    }
    return dictionary->LookupPredictiveWithLimit(input_key.c_str(),
                                                 input_key.size(),
                                                 limit,
                                                 allocator);
  }
}

void DictionaryPredictor::AggregateSuffixPrediction(
    PredictionType type,
    const ConversionRequest &request,
    Segments *segments,
    NodeAllocatorInterface *allocator,
    vector<Result> *results) const {
  if (!(type & SUFFIX)) {
    return;
  }

  DCHECK(allocator);

  size_t history_size = segments->history_segments_size();
  bool has_number_history = false;

  if (history_size) {
    const string &history_key =
        segments->history_segment(history_size - 1).key();
    has_number_history = IsNumber(history_key);
  }

  if (has_number_history && segments->conversion_segment(0).key().size() == 0) {
    const string &history_key =
        segments->history_segment(history_size - 1).key();
    vector<string> suffixes;
    GetNumberSuffixArray(history_key, &suffixes);
    DCHECK_GT(suffixes.size(), 0);
    Node *result = NULL;
    int cost = 0;

    for (vector<string>::const_iterator it = suffixes.begin();
         it != suffixes.end(); ++it) {
      // Increment cost to show the candidates in order.
      const int kSuffixPenalty = 10;

      Node *node = allocator->NewNode();
      DCHECK(node);
      node->Init();
      node->wcost = cost;
      node->key = *it;  // Filler; same as the value
      node->value = *it;
      node->lid = counter_suffix_word_id_;
      node->rid = counter_suffix_word_id_;
      node->bnext = result;
      result = node;
      results->push_back(Result(node, SUFFIX));
      cost += kSuffixPenalty;
    }
  } else {
    const Node *node = GetPredictiveNodes(
        suffix_dictionary_, "", request, *segments, allocator);
    for (; node != NULL; node = node->bnext) {
      results->push_back(Result(node, SUFFIX));
    }
  }
}

bool DictionaryPredictor::IsZipCodeRequest(const string &key) const {
  if (key.empty()) {
    return false;
  }
  const char *begin = key.data();
  const char *end = key.data() + key.size();
  size_t mblen = 0;
  while (begin < end) {
    Util::UTF8ToUCS2(begin, end, &mblen);
    if (mblen == 1 &&
        ((*begin >= '0' && *begin <= '9') || *begin == '-')) {
      // do nothing
    } else {
      return false;
    }

    begin += mblen;
  }

  return true;
}

DictionaryPredictor::PredictionType
DictionaryPredictor::GetPredictionType(const Segments &segments) const {
  if (segments.request_type() == Segments::CONVERSION) {
    VLOG(2) << "request type is CONVERSION";
    return NO_PREDICTION;
  }

  if (segments.conversion_segments_size() < 1) {
    VLOG(2) << "segment size < 1";
    return NO_PREDICTION;
  }

  const string &key = segments.conversion_segment(0).key();

  // default setting
  int result = NO_PREDICTION;

  // support realtime conversion.
  const size_t kMaxKeySize = 300;   // 300 bytes in UTF8

  const bool mixed_conversion = GET_REQUEST(mixed_conversion);

  if (segments.request_type() == Segments::PARTIAL_SUGGESTION) {
    result |= REALTIME;
  } else if ((GET_CONFIG(use_realtime_conversion) || mixed_conversion) &&
      key.size() > 0 && key.size() < kMaxKeySize) {
    result |= REALTIME;
  }

  if (!GET_CONFIG(use_dictionary_suggest) &&
      segments.request_type() == Segments::SUGGESTION) {
    VLOG(2) << "no_dictionary_suggest";
    return static_cast<PredictionType>(result);
  }

  const bool zero_query_suggestion = GET_REQUEST(zero_query_suggestion);

  const size_t key_len = Util::CharsLen(key);
  if (key_len == 0 && !zero_query_suggestion) {
    return static_cast<PredictionType>(result);
  }

  // Never trigger prediction if key looks like zip code.
  const bool is_zip_code = DictionaryPredictor::IsZipCodeRequest(key);

  if (segments.request_type() == Segments::SUGGESTION &&
      is_zip_code && key_len < 6) {
    return static_cast<PredictionType>(result);
  }

  const int kMinUnigramKeyLen = zero_query_suggestion ? 1 : 3;

  // unigram based suggestion requires key_len >= kMinUnigramKeyLen.
  // Providing suggestions from very short user input key is annoying.
  if ((segments.request_type() == Segments::PREDICTION && key_len >= 1) ||
      key_len >= kMinUnigramKeyLen) {
    result |= UNIGRAM;
  }

  const size_t history_segments_size = segments.history_segments_size();
  if (history_segments_size > 0) {
    const Segment &history_segment =
        segments.history_segment(history_segments_size - 1);
    const int kMinHistoryKeyLen = zero_query_suggestion ? 2 : 3;
    // even in PREDICTION mode, bigram-based suggestion requires that
    // the length of previous key is >= kMinBigramKeyLen.
    // It also implies that bigram-based suggestion will be triggered,
    // even if the current key length is short enough.
    // TOOD(taku): this setting might be aggressive if the current key
    // looks like Japanese particle like "が|で|は"
    // If the current key looks like particle, we can make the behavior
    // less aggressive.
    if (history_segment.candidates_size() > 0 &&
        Util::CharsLen(history_segment.candidate(0).key) >= kMinHistoryKeyLen) {
      result |= BIGRAM;
    }
  }

  if (history_segments_size > 0 && zero_query_suggestion) {
    result |= SUFFIX;
  }

  return static_cast<PredictionType>(result);
}
}  // namespace mozc
