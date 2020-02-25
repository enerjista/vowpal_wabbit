#pragma once

#include "hash.h"

// get the error code defined in master
#include "explore.h"

#include <stdint.h>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <cmath>

namespace exploration
{
const uint64_t a = 0xeece66d5deece66dULL;
const uint64_t c = 2147483647;

const int bias = 127 << 23;

union int_float {
  int32_t i;
  float f;
};

// uniform random between 0 and 1
inline float uniform_random_merand48_advance(uint64_t& initial)
{
  initial = a * initial + c;
  int_float temp;
  temp.i = ((initial >> 25) & 0x7FFFFF) | bias;
  return temp.f - 1;
}

// uniform random between 0 and 1
inline float uniform_random_merand48(uint64_t initial) { return uniform_random_merand48_advance(initial); }

template <typename It>
int generate_epsilon_greedy(
    float epsilon, uint32_t top_action, It pmf_first, It pmf_last, std::random_access_iterator_tag /* pmf_tag */)
{
  if (pmf_last < pmf_first)
    return E_EXPLORATION_BAD_RANGE;

  size_t num_actions = pmf_last - pmf_first;
  if (num_actions == 0)
    return E_EXPLORATION_BAD_RANGE;

  if (top_action >= num_actions)
    top_action = (uint32_t)num_actions - 1;

  float prob = epsilon / (float)num_actions;

  for (It d = pmf_first; d != pmf_last; ++d) *d = prob;

  *(pmf_first + top_action) += 1.f - epsilon;

  return S_EXPLORATION_OK;
}

template <typename It>
int generate_epsilon_greedy(float epsilon, uint32_t top_action, It pmf_first, It pmf_last)
{
  typedef typename std::iterator_traits<It>::iterator_category pmf_category;
  return generate_epsilon_greedy(epsilon, top_action, pmf_first, pmf_last, pmf_category());
}

template <typename InputIt, typename OutputIt>
int generate_softmax(float lambda, InputIt scores_first, InputIt scores_last, std::input_iterator_tag /* scores_tag */,
    OutputIt pmf_first, OutputIt pmf_last, std::random_access_iterator_tag /* pmf_tag */)
{
  if (scores_last < scores_first || pmf_last < pmf_first)
    return E_EXPLORATION_BAD_RANGE;

  size_t num_actions_scores = scores_last - scores_first;
  size_t num_actions_pmf = pmf_last - pmf_first;

  if (num_actions_scores != num_actions_pmf)
  {
    // fallback to the minimum
    scores_last = scores_first + ((std::min)(num_actions_scores, num_actions_pmf));
    OutputIt pmf_new_last = pmf_first + ((std::min)(num_actions_scores, num_actions_pmf));

    // zero out pmf
    for (OutputIt d = pmf_new_last; d != pmf_last; ++d) *d = 0;

    pmf_last = pmf_new_last;
  }

  if (pmf_last - pmf_first == 0)
    return E_EXPLORATION_BAD_RANGE;

  float norm = 0.;
  float max_score = *std::max_element(scores_first, scores_last);

  InputIt s = scores_first;
  for (OutputIt d = pmf_first; d != pmf_last && s != scores_last; ++d, ++s)
  {
    float prob = exp(lambda * (*s - max_score));
    norm += prob;

    *d = prob;
  }

  // normalize
  for (OutputIt d = pmf_first; d != pmf_last; ++d) *d /= norm;

  return S_EXPLORATION_OK;
}

template <typename InputIt, typename OutputIt>
int generate_softmax(float lambda, InputIt scores_first, InputIt scores_last, OutputIt pmf_first, OutputIt pmf_last)
{
  typedef typename std::iterator_traits<InputIt>::iterator_category scores_category;
  typedef typename std::iterator_traits<OutputIt>::iterator_category pmf_category;

  return generate_softmax(lambda, scores_first, scores_last, scores_category(), pmf_first, pmf_last, pmf_category());
}

template <typename InputIt, typename OutputIt>
int generate_bag(InputIt top_actions_first, InputIt top_actions_last, std::input_iterator_tag /* top_actions_tag */,
    OutputIt pmf_first, OutputIt pmf_last, std::random_access_iterator_tag /* pmf_tag */)
{
  // iterators don't support <= in general
  if (pmf_first == pmf_last || pmf_last < pmf_first)
    return E_EXPLORATION_BAD_RANGE;

  float num_models = (float)std::accumulate(top_actions_first, top_actions_last, 0.);
  if (num_models <= 1e-6)
  {
    // based on above checks we have at least 1 element in pmf
    *pmf_first = 1;
    for (OutputIt d = pmf_first + 1; d != pmf_last; ++d) *d = 0;

    return S_EXPLORATION_OK;
  }

  // divide late to improve numeric stability
  InputIt t_a = top_actions_first;
  float normalizer = 1.f / num_models;
  for (OutputIt d = pmf_first; d != pmf_last && t_a != top_actions_last; ++d, ++t_a) *d = *t_a * normalizer;

  return S_EXPLORATION_OK;
}

template <typename InputIt, typename OutputIt>
int generate_bag(InputIt top_actions_first, InputIt top_actions_last, OutputIt pmf_first, OutputIt pmf_last)
{
  typedef typename std::iterator_traits<InputIt>::iterator_category top_actions_category;
  typedef typename std::iterator_traits<OutputIt>::iterator_category pmf_category;

  return generate_bag(top_actions_first, top_actions_last, top_actions_category(), pmf_first, pmf_last, pmf_category());
}

template <typename It>
int enforce_minimum_probability(float minimum_uniform, bool update_zero_elements, It pmf_first, It pmf_last,
    std::random_access_iterator_tag /* pmf_tag */)
{
  // iterators don't support <= in general
  if (pmf_first == pmf_last || pmf_last < pmf_first)
    return E_EXPLORATION_BAD_RANGE;

  size_t num_actions = pmf_last - pmf_first;

  if (minimum_uniform > 0.999)  // uniform exploration
  {
    size_t support_size = num_actions;
    if (!update_zero_elements)
    {
      for (It d = pmf_first; d != pmf_last; ++d)
        if (*d == 0)
          support_size--;
    }

    for (It d = pmf_first; d != pmf_last; ++d)
      if (update_zero_elements || *d > 0)
        *d = 1.f / support_size;

    return S_EXPLORATION_OK;
  }

  minimum_uniform /= num_actions;
  float touched_mass = 0.;
  float untouched_mass = 0.;
  uint16_t num_actions_touched = 0;

  for (It d = pmf_first; d != pmf_last; ++d)
  {
    auto& prob = *d;
    if ((prob > 0 || (prob == 0 && update_zero_elements)) && prob <= minimum_uniform)
    {
      touched_mass += minimum_uniform;
      prob = minimum_uniform;
      ++num_actions_touched;
    }
    else
      untouched_mass += prob;
  }

  if (touched_mass > 0.)
  {
    if (touched_mass > 0.999)
    {
      minimum_uniform = (1.f - untouched_mass) / (float)num_actions_touched;
      for (It d = pmf_first; d != pmf_last; ++d)
      {
        auto& prob = *d;
        if ((prob > 0 || (prob == 0 && update_zero_elements)) && prob <= minimum_uniform)
          prob = minimum_uniform;
      }
    }
    else
    {
      float ratio = (1.f - touched_mass) / untouched_mass;
      for (It d = pmf_first; d != pmf_last; ++d)
        if (*d > minimum_uniform)
          *d *= ratio;
    }
  }

  return S_EXPLORATION_OK;
}

template <typename It>
int enforce_minimum_probability(float minimum_uniform, bool update_zero_elements, It pmf_first, It pmf_last)
{
  typedef typename std::iterator_traits<It>::iterator_category pmf_category;

  return enforce_minimum_probability(minimum_uniform, update_zero_elements, pmf_first, pmf_last, pmf_category());
}

// Warning: `seed` must be sufficiently random for the PRNG to produce uniform random values. Using sequential seeds
// will result in a very biased distribution. If unsure how to update seed between calls, merand48 (in rand48.h) can be
// used to inplace mutate it.
template <typename It>
int sample_after_normalizing(
    uint64_t seed, It pmf_first, It pmf_last, uint32_t& chosen_index, std::input_iterator_tag /* pmf_category */)
{
  if (pmf_first == pmf_last || pmf_last < pmf_first)
    return E_EXPLORATION_BAD_RANGE;
  // Create a discrete_distribution based on the returned weights. This class handles the
  // case where the sum of the weights is < or > 1, by normalizing agains the sum.
  float total = 0.f;
  for (It pmf = pmf_first; pmf != pmf_last; ++pmf)
  {
    if (*pmf < 0)
      *pmf = 0;

    total += *pmf;
  }

  // assume the first is the best
  if (total == 0)
  {
    chosen_index = 0;
    *pmf_first = 1;
    return S_EXPLORATION_OK;
  }

  float draw = total * uniform_random_merand48(seed);
  if (draw > total)  // make very sure that draw can not be greater than total.
    draw = total;

  bool index_found = false;  // found chosen action
  float sum = 0.f;
  uint32_t i = 0;
  for (It pmf = pmf_first; pmf != pmf_last; ++pmf, ++i)
  {
    sum += *pmf;
    if (!index_found && sum > draw)
    {
      chosen_index = i;
      index_found = true;
    }
    *pmf /= total;
  }

  if (!index_found)
    chosen_index = i - 1;

  return S_EXPLORATION_OK;
}

// Warning: `seed` must be sufficiently random for the PRNG to produce uniform random values. Using sequential seeds
// will result in a very biased distribution. If unsure how to update seed between calls, merand48 (in rand48.h) can be
// used to inplace mutate it.
template <typename It>
int sample_after_normalizing(uint64_t seed, It pmf_first, It pmf_last, uint32_t& chosen_index)
{
  typedef typename std::iterator_traits<It>::iterator_category pmf_category;
  return sample_after_normalizing(seed, pmf_first, pmf_last, chosen_index, pmf_category());
}

// Warning: `seed` must be sufficiently random for the PRNG to produce uniform random values. Using sequential seeds
// will result in a very biased distribution.
// If unsure how to update seed between calls, merand48 (in rand48.h) can be used to inplace mutate it.
template <typename It>
int sample_after_normalizing(
    const char* seed, It pmf_first, It pmf_last, uint32_t& chosen_index, std::random_access_iterator_tag pmf_category)
{
  uint64_t seed_hash = uniform_hash(seed, strlen(seed), 0);
  return sample_after_normalizing(seed_hash, pmf_first, pmf_last, chosen_index, pmf_category);
}

// Warning: `seed` must be sufficiently random for the PRNG to produce uniform random values. Using sequential seeds
// will result in a very biased distribution. If unsure how to update seed between calls, merand48 (in rand48.h) can be
// used to inplace mutate it.
template <typename It>
int sample_after_normalizing(const char* seed, It pmf_first, It pmf_last, uint32_t& chosen_index)
{
  typedef typename std::iterator_traits<It>::iterator_category pmf_category;
  return sample_after_normalizing(seed, pmf_first, pmf_last, chosen_index, pmf_category());
}

//
template <typename ActionIt>
int swap_chosen(
    ActionIt action_first, ActionIt action_last, std::forward_iterator_tag /* action_category */, uint32_t chosen_index)
{
  if (action_last < action_first)
    return E_EXPLORATION_BAD_RANGE;

  size_t action_size = action_last - action_first;

  if (action_size == 0)
    return E_EXPLORATION_BAD_RANGE;

  if (chosen_index >= action_size)
    return E_EXPLORATION_BAD_RANGE;

  // swap top element with chosen one
  if (chosen_index != 0)
  {
    std::iter_swap(action_first, action_first + chosen_index);
  }

  return S_EXPLORATION_OK;
}

//
template <typename ActionsIt>
int swap_chosen(ActionsIt action_first, ActionsIt action_last, uint32_t chosen_index)
{
  typedef typename std::iterator_traits<ActionsIt>::iterator_category actionit_category;
  return swap_chosen(action_first, action_last, actionit_category(), chosen_index);
}

// Pick a discrete action in proportion to the scores.
// Notes:
// 1) Random seed is advanced
// 2) Does not normalize the scores (unlike sample_after_normalization)
// 3) Scores need not add up to one.
template <typename It>
int sample_scores(uint64_t* p_seed, It scores_first, It scores_last, uint32_t& chosen_index,
    std::random_access_iterator_tag scores_category)
{
  if (scores_first == scores_last || scores_last < scores_first)
    return E_EXPLORATION_BAD_RANGE;
  // Create a discrete_distribution based on the returned weights. This class handles the
  // case where the sum of the weights is < or > 1, by normalizing agains the sum.
  float total = 0.f;
  for (It scores = scores_first; scores != scores_last; ++scores)
  {
    if (*scores < 0)
      *scores = 0;

    total += *scores;
  }

  // assume the first is the best
  if (total == 0)
  {
    chosen_index = 0;
    *scores_first = 1;
    return S_EXPLORATION_OK;
  }

  float draw = total * uniform_random_merand48_advance(*p_seed);
  if (draw > total)  // make very sure that draw can not be greater than total.
    draw = total;

  float sum = 0.f;
  uint32_t i = 0;
  for (It scores = scores_first; scores != scores_last; ++scores, ++i)
  {
    sum += *scores;
    if (sum > draw)
    {
      chosen_index = i;
      return S_EXPLORATION_OK;
    }
  }

  chosen_index = i - 1;
  return S_EXPLORATION_OK;
}

// Draw a random number between [range_min, range_max * edge_avoid_factor]
// and advance pseudo-random state
inline float internal_interval_draw(uint64_t* p_seed, float range_min, float range_max, float edge_avoid_factor)
{
  // Draw a float and then advance the pseudo-random state
  const float random_draw = edge_avoid_factor * uniform_random_merand48_advance(*p_seed);
  const float interval_size = (range_max - range_min);
  const float chosen_value = interval_size * random_draw + range_min;
  return chosen_value;
}

// Draw a random number between [range_min, range_max)
// and advance pseudo-random state
float inline uniform_draw(uint64_t* p_seed, float range_min, float range_max)
{
  float chosen_value;

  do
  {
    const float edge_avoid_factor = 1.0001f;
    chosen_value = internal_interval_draw(p_seed, range_min, range_max, edge_avoid_factor);
  } while (chosen_value >= range_max);

  return chosen_value;
}

// Sample one action from the given probability density function.
template <typename It>
int sample_pdf(uint64_t* p_seed, It pdf_first, It pdf_last, float range_min, float range_max, float& chosen_value,
    std::random_access_iterator_tag pdf_category)
{
  // Pick the index of chosen segment index treating the pdf as a collection of scores
  // Random seed is advanced.
  uint32_t chosen_index;
  auto err_code = sample_scores(seed, pdf_first, pdf_last, chosen_index, pdf_category);
  if (err_code != S_EXPLORATION_OK)
    return err_code;

  // Pick an action value in the chosen range
  size_t num_intervals = pdf_last - pdf_first;

  // By convention pdf contains 0.f and the value for end of the interval
  // Adjust for this, but check first (just in case).
  if (*(pdf_first+(num_intervals-1)) == 0.f)
  {
    --num_intervals;
  }

  const float interval_size = (range_max - range_min) / num_intervals;
  const float interval_start = range_min + interval_size * chosen_index;
  const float interval_end = (std::min)(range_max, interval_start + interval_size);

  // Choose a random value within the chosen interval
  chosen_value = uniform_draw(p_seed, interval_start, interval_end);

  return S_EXPLORATION_OK;
}

// Warning: `seed` must be sufficiently random for the PRNG to produce uniform random values. Using sequential seeds
// will result in a very biased distribution. If unsure how to update seed between calls, merand48 (in rand48.h) can
// be used to inplace mutate it.
template <typename It>
int sample_pdf(uint64_t* p_seed, It pdf_first, It pdf_last, float min_value, float max_value, float& chosen_value)
{
  typedef typename std::iterator_traits<It>::iterator_category pdf_category;
  return sample_pdf(p_seed, pdf_first, pdf_last, min_value, max_value, chosen_value, pdf_category());
}

}  // namespace exploration
