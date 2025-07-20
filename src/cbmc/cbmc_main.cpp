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
#include "solvers/sat/satcheck_ipasir.h"

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
  auto start_time = std::chrono::steady_clock::now();
  for(int i = 0; i < argc; ++i)
  {
    std::cout << "argv[" << i << "]: " << argv[i] << std::endl;
  }

  cbmc_parse_optionst parse_options(argc, argv);
  int res = parse_options.main();

  auto end_time = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  std::cout << "t PROCESSING_TIME: " << duration.count() / 1000.0f << std::endl;
  std::cout << "t SAT_TIME: " << satcheck_ipasirt::sat_time << std::endl;
  std::cout << "t SAT_CALLS: " << satcheck_ipasirt::sat_calls << std::endl;
  std::cout << "s EC=" << res << std::endl;

  #ifdef IREP_HASH_STATS
  std::cout << "IREP_HASH_CNT=" << irep_hash_cnt << '\n';
  std::cout << "IREP_CMP_CNT=" << irep_cmp_cnt << '\n';
  std::cout << "IREP_CMP_NE_CNT=" << irep_cmp_ne_cnt << '\n';
  #endif

  return res;
}
