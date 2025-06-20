/*******************************************************************\

Module: CBMC Main Module

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// CBMC Main Module

/*

  CBMC
  Bounded Model Checking for ANSI-C
  Copyright (C) 2001-2014 Daniel Kroening <kroening@kroening.com>

*/

#include "cbmc_parse_options.h"
#include <chrono>
#include <iostream>

#ifdef _MSC_VER
#  include <util/unicode.h>
#endif

#ifdef IREP_HASH_STATS
#include <iostream>
#endif

#ifdef IREP_HASH_STATS
extern unsigned long long irep_hash_cnt;
extern unsigned long long irep_cmp_cnt;
extern unsigned long long irep_cmp_ne_cnt;
#endif

#ifdef _MSC_VER
int wmain(int argc, const wchar_t **argv_wide)
{
  auto vec=narrow_argv(argc, argv_wide);
  auto narrow=to_c_str_array(std::begin(vec), std::end(vec));
  auto argv=narrow.data();
#else
int main(int argc, const char **argv)
{
#endif
  class TimeLogger
  {
  public:
    std::chrono::steady_clock::time_point &start, &end;
    TimeLogger(std::chrono::steady_clock::time_point &s,
                  std::chrono::steady_clock::time_point &e)
      : start(s), end(e) {}
    ~TimeLogger()
    {
      std::cout << "t CBMCTIME:"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                        end - start)
                          .count()
                  << " ms" << std::endl;
    }
  };

  auto start = std::chrono::steady_clock::now();
  auto end = start;

  TimeLogger sat_time_logger(start, end);


  cbmc_parse_optionst parse_options(argc, argv);
  int res = -1;

  try
  {
    res = parse_options.main();
    end = std::chrono::steady_clock::now();
  }
    catch(...)
  {
    end = std::chrono::steady_clock::now();
    throw;
  }

  #ifdef IREP_HASH_STATS
  std::cout << "IREP_HASH_CNT=" << irep_hash_cnt << '\n';
  std::cout << "IREP_CMP_CNT=" << irep_cmp_cnt << '\n';
  std::cout << "IREP_CMP_NE_CNT=" << irep_cmp_ne_cnt << '\n';
  #endif

  return res;
}
