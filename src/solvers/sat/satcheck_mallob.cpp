#include "satcheck_mallob.h"

#include <util/exception_utils.h>
#include <util/invariant.h>
#include <util/narrow.h>
#include <util/threeval.h>

//#ifdef HAVE_MALLOB

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>
#include <iostream>
#include <algorithm>
#include <string>
#include <exception>
#include <initializer_list>
#include <list>
#include <memory>
#include <thread>
#include <vector>

#include "util/logger.hpp"
#include "util/params.hpp"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "util/json.hpp"
#include "util/option.hpp"
#include "interface/api/api_registry.hpp"
#include "app/2ls/cbmc_sat_connector.hpp"

#include "util/sys/timer.hpp"

//int satcheck_mallobt::streamIdCounter = 0;
//APIConnector* satcheck_mallobt::_api = nullptr;

//bool pending = false;
//nlohmann::json result_json;
//int job_id = 0;

// std::vector<int> decompressModel(const std::string& compressedModel) {
//   char* solutionStr;
//   size_t nbVars = std::strtoul(compressedModel.c_str(), &solutionStr, 10); // reads until ":"
//   assert(solutionStr[0] == ':');
//   std::vector<int> solution(nbVars+1, 0); // index 0 has a filler 0

//   int strpos = 1; // after ":"
//   int var = 1;
//   while (solutionStr[strpos] != '\0') {
//       char c = solutionStr[strpos];
//       std::string cAsString(1, c);
//       char* endptr;
//       int num = std::strtol(cAsString.c_str(), &endptr, 16);
//       assert(endptr - cAsString.c_str() == 1); // read exactly one character!
//       if (var <= nbVars) solution[var] = (num & 1) ? var : -var;
//       var++;
//       if (var <= nbVars) solution[var] = (num & 2) ? var : -var;
//       var++;
//       if (var <= nbVars) solution[var] = (num & 4) ? var : -var;
//       var++;
//       if (var <= nbVars) solution[var] = (num & 8) ? var : -var;
//       var++;
//       strpos++;
//   }
//   //LOG(V2_INFO, "MAXSAT DECOMPRESS %s ==> %s\n", packed.c_str(), StringUtils::getSummary(solution, INT_MAX).c_str());

//   //printf("(%.3f) Decompressed model to size %lu\n", Timer::elapsedSeconds(), solution.size());
//   return solution;
// }


satcheck_mallobt::satcheck_mallobt(message_handlert &message_handler)
  : cnf_solvert(message_handler)
{
  // if (_api == nullptr) {
  //   _api = APIRegistry::get();
  // }

  // std::cout << "Hello" << _api->active() << std::endl; 
  // //std::cout << "Hello from satcheck_mallobt" << std::endl;
  // _streamer = new SatJobStream(*_api, streamIdCounter, true);

  // std::cout << "Created SatJobStream with ID: " << streamIdCounter << std::endl;
  // streamIdCounter++;

  _sat_connector = new CBMCSatConnector("Mallob SAT Connector");
}

satcheck_mallobt::~satcheck_mallobt() { 
  // if (_submitted) {
  //   _streamer->finalize();
  //   std::cout << "SatJobStream finalized" << std::endl;
  // }
  //_sat_connector->setTerminate();
  _model.clear();
  _failed_assumptions.clear();
  _formula.clear();
  delete _sat_connector;
  _sat_connector = nullptr; 

  //_api = nullptr;
  //_streamer = nullptr;

  std::cout << "SAT checker: instance is deleted" << std::endl;
}

const std::string satcheck_mallobt::solver_text() 
{
  return "Mallob SAT Solver";
}

tvt satcheck_mallobt::l_get(literalt a) const
{
  if(a.is_true()) 
    return tvt(true);
  else if(a.is_false()) 
    return tvt(false);

  tvt result;

  // Check if the variable is in bounds
  //if(a.var_no() >= _model.size())
  if(a.var_no()>=(unsigned)no_variables())
    return tvt::unknown();

  //std::cout << "Comparison: " << a.dimacs() << " with var no " << a.var_no() 
  //<< "and sign " << a.sign() << std::endl;    
  const int val = _model[a.var_no()];
  if(val > 0)
    result = tvt(true);
  else if(val < 0)
    result = tvt(false);
  else
    return tvt::unknown();

  if(a.sign()) // a negative
    result = !result;

  return result;
}

void satcheck_mallobt::lcnf(const bvt &bv)
{
  for(const auto &lit : bv)
  {
    if(lit.is_true())
      return;
    else if(!lit.is_false())
      INVARIANT(lit.var_no() < no_variables(), 
      "reject out of bound variables");
  }

  //std::cout << "Adding clause: ";
  for(const auto &literal : bv)
  {
    if(!literal.is_false())
    {
      // add literal with correct sign
      _formula.push_back(literal.dimacs());
      //std::cout << literal.dimacs() << " ";
    }
  } 
  
  if (_formula.back() == 0) {
    _empty_clause = true;
    return;
  }
  //std::cout << "0" << std::endl;
  _formula.push_back(0); // terminate clause

  if(solver_hardness)
  {
    // To map clauses to lines of program code, track clause indices in the
    // dimacs cnf output. Dimacs output is generated after processing
    // clauses to remove duplicates and clauses that are trivially true.
    static size_t cnf_clause_index = 0;
    bvt cnf;
    bool clause_removed = process_clause(bv, cnf);

    if(!clause_removed)
      cnf_clause_index++;

    solver_hardness->register_clause(
      bv, cnf, cnf_clause_index, !clause_removed);
  }

  clause_counter++;
}

void satcheck_mallobt::set_assignment(literalt a, bool value)
{
  INVARIANT(!a.is_constant(), "cannot set an assignment for a constant");
  INVARIANT(false, "method not supported");
}

void satcheck_mallobt::set_assumptions(const bvt &bv)
{
  assumptions.clear();
  //std::cout << "Assumptions: ";
  for(const auto &assumption : bv)
  {
    if(!assumption.is_true())
    {
      //std::cout << assumption.dimacs() << " ";
      assumptions.push_back(assumption);
    }
  }
  //std::cout << std::endl;
}

bool satcheck_mallobt::is_in_conflict(literalt a) const
{
  // Check if this literal is in the failed assumptions list
  //std::cout << "Checking if " << a.dimacs() << " is in conflict: ";
  //for (const auto &fa : _failed_assumptions)
  //{
  //  std::cout << fa;
  //}
  //std::cout << std::endl;
  return _failed_assumptions.count(a.dimacs());
}

propt::resultt satcheck_mallobt::do_prop_solve()
{
  //assert(!_streamer->isPending());
  std::cout << "Entered prop solve" << std::endl;

  INVARIANT(status != statust::ERROR, "there cannot be an error");

  std::cout << (no_variables() - 1) << " variables, " << clause_counter
                   << " clauses" << std::endl;

  if (_empty_clause) {
    std::cout << "There was an empty clause" << std::endl;
    status = statust::UNSAT;
    return resultt::P_UNSATISFIABLE;
  }

  std::vector<int> currAssumptions;
  currAssumptions.reserve(assumptions.size());
  // Check for trivial UNSAT from assumptions
  for(const auto &a : assumptions)
  {
    if(a.is_false())
    {
      std::cout << "got FALSE as assumption: instance is UNSATISFIABLE"
                  << std::endl;
      
      status = statust::UNSAT;
      return resultt::P_UNSATISFIABLE;
    } else {
      currAssumptions.push_back(a.dimacs());
    }
  }

  _sat_connector->setFormula(std::move(_formula), no_variables(), no_clauses());
  _formula.clear(); 
  _sat_connector->setAssumptions(std::move(currAssumptions));
  int resultCode = _sat_connector->solve();

  if (resultCode == 10) {
    // SAT
    _model = _sat_connector->getSolution();
    std::cout << "SAT checker: instance is SATISFIABLE" << std::endl;
    status = statust::SAT;
    return resultt::P_SATISFIABLE;

  } else if (resultCode == 20) {
    // UNSAT
    _failed_assumptions = _sat_connector->getFailedLiterals();
    std::cout << "SAT checker: instance is UNSATISFIABLE" << std::endl;
    status = statust::UNSAT;
    return resultt::P_UNSATISFIABLE;

  } else {
    status = statust::ERROR;
    return resultt::P_ERROR;
  }
  
  
  
  // _streamer->submitNext(std::move(_formula), currAssumptions, "", 1.0);
  // _submitted = true;
  // std::cout << "Mallob job submitted" << std::endl;
  // while (_streamer->isPending()) {
  //   std::cout << "Waiting for job to finish" << std::endl;
  //   usleep(10000);
  // }
  // _formula = std::vector<int>();
  // nlohmann::json j = _streamer->getResult();

  // std::cout << "Mallob job finished" << std::endl;  
  
  // int resultcode;
  // // Success!
  // resultcode = j["result"]["resultcode"];
  // if (resultcode == 10) {
  //   //_model.clear();
  //     // SAT
  //   _model.resize(no_variables()+1, 0);

  //   if (j["result"]["solution"].is_array()) {
  //     if (j["result"]["solution"].size() > 0 && j["result"]["solution"][0].is_number()) {
  //       std::vector<int> modelLits = j["result"]["solution"].get<std::vector<int>>();
  //       //std::cout << Timer::elapsedSeconds() << " Got model" << modelLits.size() << std::endl;
  //       for (int lit : modelLits) {
  //         const int var = std::abs(lit);
  //         _model[var] = lit;
  //       }
  //     } 
  //   } else if (j["result"]["solution"].is_string()) {
  //     // Handle single compressed model string
  //     std::string compressedModel = j["result"]["solution"].get<std::string>();
  //     //std::cout << Timer::elapsedSeconds() << " Got compressed model" << compressedModel.c_str() << std::endl;
  //     _model = decompressModel(compressedModel);
  //   }

  //   j["result"]["solution"] = "[solution data omitted]";
  //   LOG(V2_INFO, "Mallob result: %s\n", j.dump().c_str());
    
  //   std::cout << "SAT checker: instance is SATISFIABLE" << std::endl;
  //   status = statust::SAT;
  //   return resultt::P_SATISFIABLE;

  // } else if (resultcode == 20) {
  //   _failed_assumptions.clear();
  //     // UNSAT
  //     // Check the type of the solution field
  //     if (j["result"]["solution"].is_array()) {
  //       // Handle as array of integers
  //       if (j["result"]["solution"].size() > 0 && j["result"]["solution"][0].is_number()) {
  //         std::vector<int> failedAssumptions = j["result"]["solution"].get<std::vector<int>>();
  //         //std::cout << Timer::elapsedSeconds() << " Got direct integer solution of size" << failedAssumptions.size() << std::endl;
  //         _failed_assumptions.insert(failedAssumptions.begin(), failedAssumptions.end());
  //       }
  //     }

  //   LOG(V2_INFO, "Mallob result: %s\n", j.dump().c_str());
    
  //   std::cout << "SAT checker: instance is UNSATISFIABLE" << std::endl;
  //   status = statust::UNSAT;
  //   return resultt::P_UNSATISFIABLE;
  // } else {
  //   status = statust::ERROR;
  //   return resultt::P_ERROR;
  // }
       
  // // DUMMY IMPLEMENTATION: Always return SAT
  // std::cout << "SAT checker (DUMMY): instance is SATISFIABLE" << std::endl;
  
  // status = statust::SAT;
  // return resultt::P_SATISFIABLE;
}
//#endif