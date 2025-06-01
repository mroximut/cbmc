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
#include "app/cbmc/sat_job_stream.hpp"
#include "interface/api/api_registry.hpp"


//bool pending = false;
nlohmann::json result_json;
//int job_id = 0;


std::vector<int> decompressModel(const std::string& compressedModel) {
  char* solutionStr;
  size_t nbVars = std::strtoul(compressedModel.c_str(), &solutionStr, 10); // reads until ":"
  assert(solutionStr[0] == ':');
  std::vector<int> solution(nbVars+1, 0); // index 0 has a filler 0

  int strpos = 1; // after ":"
  int var = 1;
  while (solutionStr[strpos] != '\0') {
      char c = solutionStr[strpos];
      std::string cAsString(1, c);
      char* endptr;
      int num = std::strtol(cAsString.c_str(), &endptr, 16);
      assert(endptr - cAsString.c_str() == 1); // read exactly one character!
      if (var <= nbVars) solution[var] = (num & 1) ? var : -var;
      var++;
      if (var <= nbVars) solution[var] = (num & 2) ? var : -var;
      var++;
      if (var <= nbVars) solution[var] = (num & 4) ? var : -var;
      var++;
      if (var <= nbVars) solution[var] = (num & 8) ? var : -var;
      var++;
      strpos++;
  }
  //LOG(V2_INFO, "MAXSAT DECOMPRESS %s ==> %s\n", packed.c_str(), StringUtils::getSummary(solution, INT_MAX).c_str());

  //printf("(%.3f) Decompressed model to size %lu\n", Timer::elapsedSeconds(), solution.size());
  return solution;
}


satcheck_mallobt::satcheck_mallobt(message_handlert &message_handler)
  : cnf_solvert(message_handler)
{
  _api = APIRegistry::get();
  log.status() << "Hello" << _api->active() << messaget::eom; 
  _streamer = new SatJobStream(*_api, 0, true);
}

satcheck_mallobt::~satcheck_mallobt() { 
  _streamer->finalize();

  _model.clear();
  _failed_assumptions.clear();
  _formula.clear();

  _api = nullptr;
  _streamer = nullptr;

  log.status() << "SAT checker: instance is deleted" << messaget::eom;
}

std::string satcheck_mallobt::solver_text() const
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
  if(a.var_no() >= _model.size())
    return tvt::unknown();

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

  for(const auto &literal : bv)
  {
    if(!literal.is_false())
    {
      // add literal with correct sign
      _formula.push_back(literal.dimacs());
    }
  } 
  
  if (_formula.back() == 0) {
    status = statust::UNSAT;
    return;
  }
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

bool satcheck_mallobt::is_in_conflict(literalt a) const
{
  // Check if this literal is in the failed assumptions list
  return _failed_assumptions.count(a.dimacs());
}

propt::resultt satcheck_mallobt::do_prop_solve(const bvt &assumptions)
{
  assert(!_streamer->isPending());
  log.status() << "Entered prop solve" << messaget::eom;
  _failed_assumptions.clear();

  INVARIANT(status != statust::ERROR, "there cannot be an error");

  log.statistics() << (no_variables() - 1) << " variables, " << clause_counter
                   << " clauses" << messaget::eom;

  if (status == statust::UNSAT) {
    log.status() << "There was an empty clause" << messaget::eom;
    _formula.clear();
    return resultt::P_UNSATISFIABLE;
  }

  std::vector<int> currAssumptions;
  // Check for trivial UNSAT from assumptions
  for(const auto &a : assumptions)
  {
    if(a.is_false())
    {
      log.status() << "got FALSE as assumption: instance is UNSATISFIABLE"
                  << messaget::eom;
      status = statust::UNSAT;
      _formula.clear();
      return resultt::P_UNSATISFIABLE;
    } else if (!a.is_true()) {
      currAssumptions.push_back(a.dimacs());
    }
  }

  // while (!initialized) {
  //     usleep(100000);
  // }
/////////////////////////////////////////////////////
  // pending = true;

  // std::string precursor1 = job_id == 0 ? "" : "cbmc.cbmc-job-rev." + std::to_string(job_id-1);
  //   // Write a job JSON for the singular job to solve
  //   nlohmann::json json1 = {
  //     {"user", "cbmc"},
  //     {"name", "cbmc-job-rev." + std::to_string(job_id)},
  //     {"incremental", true},
  //     //{"precursor", precursor},
  //     //{"done", true},
  //     //{"files", {"/home/oguz/Desktop/hiwi_code/cbmc_mallob_monolithic/mallob/instances/r3unknown_10k.cnf"}},
  //     {"literals", _formula},
  //     {"assumptions", {-1}},
  //     {"priority", 1.000},
  //     {"application", "SAT"}
  // };
  // if (precursor1 != "") json1["precursor"] = precursor1;
  // // if (_params->crossJobCommunication()) json["group-id"] = "1";
  // // if (_params->jobWallclockLimit() > 0)
  // //     json["wallclock-limit"] = std::to_string(_params->jobWallclockLimit()) + "s";
  // // if (_params->jobCpuLimit() > 0) {
  // //     json["cpu-limit"] = std::to_string(_params->jobCpuLimit()) + "s";
  // // }
 
  // auto result1 = _client->getAPI().submit(json1, [&](nlohmann::json& response) {
  //     job_id++;
  //     result_json = std::move(response);
  //     pending = false;
  // });
  // if (result1 != JsonInterface::Result::ACCEPT) {
  //     LOG(V0_CRIT, "[ERROR] Cannot introduce job!\n");
  //     abort();
  // }

  // while (pending) {
  //     usleep(100000);
  // }

  // pending = true;

  // std::string precursor = job_id == 0 ? "" : "cbmc.cbmc-job-rev." + std::to_string(job_id-1);
  //   // Write a job JSON for the singular job to solve
  //   nlohmann::json json = {
  //     {"user", "cbmc"},
  //     {"name", "cbmc-job-rev." + std::to_string(job_id)},
  //     {"incremental", true},
  //     //{"precursor", precursor},
  //     //{"done", true},
  //     //{"files", {"/home/oguz/Desktop/hiwi_code/cbmc_mallob_monolithic/mallob/instances/r3unknown_10k.cnf"}},
  //     {"literals", _formula},
  //     {"assumptions", currAssumptions},
  //     {"priority", 1.000},
  //     {"application", "SAT"}
  // };
  //if (precursor != "") json["precursor"] = precursor;
  // if (_params->crossJobCommunication()) json["group-id"] = "1";
  // if (_params->jobWallclockLimit() > 0)
  //     json["wallclock-limit"] = std::to_string(_params->jobWallclockLimit()) + "s";
  // if (_params->jobCpuLimit() > 0) {
  //     json["cpu-limit"] = std::to_string(_params->jobCpuLimit()) + "s";
  // }
 
  // auto result = _api->submit(json, [&](nlohmann::json& response) {
  //     job_id++;
  //     result_json = std::move(response);
  //     pending = false;
  // });
  // if (result != JsonInterface::Result::ACCEPT) {
  //     LOG(V0_CRIT, "[ERROR] Cannot introduce job!\n");
  //     abort();
  // }

  // while (pending) {
  //     usleep(100000);
  // }
  /////////////////////////////////////////
  
  _streamer->submitNext(std::move(_formula), currAssumptions, "", 1.0);
  while (_streamer->isPending()) {
    //log.status() << "Waiting for job to finish" << messaget::eom;
    usleep(100000);
  }
  _formula = std::vector<int>();
  result_json = _streamer->getResult();

  log.status() << "Mallob job finished" << messaget::eom;  
  if (!result_json.empty()) {
    LOG(V2_INFO, "Result: %s\n", result_json.dump().c_str());
  }
  
  int resultcode;
  nlohmann::json j = result_json;
  // Success!
  resultcode = j["result"]["resultcode"];
  if (resultcode == 10) {
      // SAT
    _model.resize(no_variables()+1, 0);

    if (j["result"]["solution"].is_array()) {
      if (j["result"]["solution"].size() > 0 && j["result"]["solution"][0].is_number()) {
        std::vector<int> modelLits = j["result"]["solution"].get<std::vector<int>>();
        //log.status() << Timer::elapsedSeconds() << " Got model" << modelLits.size() << messaget::eom;
        for (int lit : modelLits) {
          const int var = std::abs(lit);
          _model[var] = lit;
        }
      } 
    } else if (j["result"]["solution"].is_string()) {
      // Handle single compressed model string
      std::string compressedModel = j["result"]["solution"].get<std::string>();
      //log.status() << Timer::elapsedSeconds() << " Got compressed model" << compressedModel.c_str() << messaget::eom;
      _model = decompressModel(compressedModel);
    }

    log.status() << "SAT checker: instance is SATISFIABLE" << messaget::eom;
    status = statust::SAT;
    return resultt::P_SATISFIABLE;

  } else if (resultcode == 20) {
      // UNSAT
      // Check the type of the solution field
      if (j["result"]["solution"].is_array()) {
        // Handle as array of integers
        if (j["result"]["solution"].size() > 0 && j["result"]["solution"][0].is_number()) {
          std::vector<int> failedAssumptions = j["result"]["solution"].get<std::vector<int>>();
          log.status() << Timer::elapsedSeconds() << " Got direct integer solution of size" << failedAssumptions.size() << messaget::eom;
          _failed_assumptions.insert(failedAssumptions.begin(), failedAssumptions.end());
        }
      }
    log.status() << "SAT checker: instance is UNSATISFIABLE" << messaget::eom;
    status = statust::UNSAT;
    return resultt::P_UNSATISFIABLE;
  } else {
    status = statust::ERROR;
    return resultt::P_ERROR;
  }
       
  // DUMMY IMPLEMENTATION: Always return SAT
  //log.status() << "SAT checker (DUMMY): instance is SATISFIABLE" << messaget::eom;
  
  //status = statust::SAT;
  //return resultt::P_SATISFIABLE;
}
//#endif


