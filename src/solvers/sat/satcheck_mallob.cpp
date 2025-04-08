#include "satcheck_mallob.h"

#include <util/exception_utils.h>
#include <util/invariant.h>
#include <util/narrow.h>
#include <util/threeval.h>

//#ifdef HAVE_MALLOB
// #include "util/params.hpp"               
// #include "interface/api/api_connector.hpp"
// #include "data/job_description.hpp"     
// #include "util/logger.hpp"               
// #include "interface/json_interface.hpp" 
// #include "util/json.h"
// #include <unistd.h>
#include "app/sat/register.hpp"
// #include "core/client.hpp"
// #include "core/worker.hpp"

// #include <mpi.h>
// #include "util/sys/timer.hpp"
// #include "util/sys/tmpdir.hpp"
// #include "util/sys/process.hpp"

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

#include "comm/distributed_termination.hpp"
#include "comm/mympi.hpp"
#include "interface/api/rank_specific_file_fetcher.hpp"
#include "util/periodic_event.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/params.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "core/worker.hpp"
#include "core/client.hpp"
#include "util/sys/thread_pool.hpp"
#include "interface/api/job_streamer.hpp"
#include "comm/host_comm.hpp"
#include "data/job_transfer.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "util/sys/tmpdir.hpp"
#include "comm/mpi_base.hpp"
#include "comm/msg_queue/message_handle.hpp"
#include "comm/msg_queue/message_queue.hpp"
#include "comm/msgtags.h"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "util/json.hpp"
#include "util/option.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/terminator.hpp"
//#include "app/.register_includes.h"

#ifndef MALLOB_VERSION
#define MALLOB_VERSION "(dbg)"
#endif

bool pending = false;
bool allDone = false;
nlohmann::json result_json;
bool started = false;
bool initialized = false;
bool terminated = false;
int job_id = 0;


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

  printf("(%.3f) Decompressed model to size %lu\n", Timer::elapsedSeconds(), solution.size());
  return solution;
}

int satcheck_mallobt::main_mallob(int argc, char *argv[]) {
  
  started = true;
  MyMpi::init();
  Timer::init();
  Proc::nameThisThread("MainThread");

  int numNodes = MyMpi::size(MPI_COMM_WORLD);
  int rank = MyMpi::rank(MPI_COMM_WORLD);

  Parameters params;

  params.init(argc, argv);
  for (int i = 0; i < argc; i++) {
      LOG(V2_INFO, "argv[%d]: %s\n", i, argv[i]);
  }
  if (rank == 0) params.printBanner();

  // Initialize bookkeeping of child processes and signals
  Process::init(rank, params.traceDirectory());
  TmpDir::init(rank, params.tmpDirectory());

  Logger::LoggerConfig logConfig;
  logConfig.rank = rank;
  logConfig.verbosity = params.verbosity();
  logConfig.coloredOutput = params.coloredOutput();
  logConfig.flushFileImmediately = params.immediateFileFlush();
  logConfig.quiet = params.quiet();
  if (params.zeroOnlyLogging() && rank > 0) logConfig.quiet = true;
  logConfig.cPrefix = params.monoFilename.isSet();
  std::string logDirectory = params.logDirectory();
  std::string logFilename = "log." + std::to_string(rank);
  logConfig.logDirOrNull = logDirectory.empty() ? nullptr : &logDirectory;
  logConfig.logFilenameOrNull = &logFilename;
  Logger::init(logConfig);


  MyMpi::setOptions(params);

  // Register all applications which were compiled into Mallob
  //#include "app/.register_commands.h"
  register_mallob_app_sat();

  if (rank == 0)
      LOG(V2_INFO, "Program options: %s\n", params.getParamsAsString().c_str());
  if (params.help()) {
      // Help requested or no job input provided
      if (rank == 0) {
          params.printUsage();
      }
      MPI_Finalize();
      Process::doExit(0);
  }

  char hostname[1024];
  gethostname(hostname, 1024);
  LOG(V3_VERB, "Mallob %s pid=%lu on host %s\n", MALLOB_VERSION, Proc::getPid(), hostname);

  // Global and local seed, such that all nodes have access to a synchronized randomness
  // as well as to an individual randomness that differs among nodes
  Random::init(numNodes+params.seed(), rank+params.seed());

  std::unique_ptr<DistributedTermination> distTerm (new DistributedTermination()); // RAII

  auto isWorker = [&](int rank) {
      if (params.numWorkers() == -1) return true; 
      return rank < params.numWorkers();
  };
  auto isClient = [&](int rank) {
      if (params.monoFilename.isSet()) return rank == 0;
      if (params.numClients() == -1) return true;
      return rank >= numNodes - params.numClients();
  };

  // Create communicators for clients and for workers
  std::vector<int> clientRanks;
  std::vector<int> workerRanks;
  for (int i = 0; i < numNodes; i++) {
      if (isWorker(i)) workerRanks.push_back(i);
      if (isClient(i)) clientRanks.push_back(i);
  }
  if (rank == 0) LOG(V3_VERB, "%i workers, %i clients\n", workerRanks.size(), clientRanks.size());

  // Initialize thread pool
  int threadPoolSize = 4;
  if (isClient(rank) && isWorker(rank)) threadPoolSize *= 2;
  ProcessWideThreadPool::init(threadPoolSize);
  
  MPI_Comm clientComm, workerComm;
  {
      MPI_Group worldGroup;
      MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
      MPI_Group clientGroup;
      MPI_Group_incl(worldGroup, clientRanks.size(), clientRanks.data(), &clientGroup);
      MPI_Comm_create(MPI_COMM_WORLD, clientGroup, &clientComm);
  }
  if (rank == 0) LOG(V3_VERB, "Created client communicator\n");
  {
      MPI_Group worldGroup;
      MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
      MPI_Group workerGroup;
      MPI_Group_incl(worldGroup, workerRanks.size(), workerRanks.data(), &workerGroup);
      MPI_Comm_create(MPI_COMM_WORLD, workerGroup, &workerComm);
  }
  if (rank == 0) LOG(V3_VERB, "Created worker communicator\n");
  
  // Execute main program
  try {
      //doMainProgram(workerComm, clientComm, params, *distTerm.get());
          // Determine which role(s) this PE has
    auto& commWorkers = workerComm;
    auto& commClients = clientComm;
    auto& distTerm2 = *distTerm.get();
    DistributedTermination& distTerm = distTerm2;

    bool isWorker = commWorkers != MPI_COMM_NULL;
    bool isClient = commClients != MPI_COMM_NULL;
    if (isWorker) LOG(V4_VVER, "I am worker #%i\n", MyMpi::rank(commWorkers));
    if (isClient) LOG(V4_VVER, "I am client #%i\n", MyMpi::rank(commClients));

    // Create worker and client as necessary
    Worker* worker = isWorker ? new Worker(commWorkers, params) : nullptr;
    Client* client = isClient ? new Client(commClients, params) : nullptr;
    
    // Initialize worker and client as necessary (background threads, callbacks, ...)
    if (isWorker) worker->init();
    if (isClient) client->init();
    int myRank = MyMpi::rank(MPI_COMM_WORLD);

    // Deposit information to coordinate the creation of an intra-machine communicator
    HostComm hostComm(commWorkers, params);
    hostComm.depositInformation();

    LOG(V5_DEBG, "Global init barrier ...\n");
    MPI_Barrier(MPI_COMM_WORLD);
    LOG(V5_DEBG, "Passed global init barrier\n");

    // Create intra-machine communicator (collective operation)
    hostComm.create();
    if (isWorker) worker->setHostComm(hostComm);

    // If mono solving mode is enabled, introduce the singular job to solve
    //if (/*params.monoFilename.isSet() &&*/ isClient && MyMpi::rank(commClients) == 0)
    //    introduceMonoJob(params, *client);
    _params = &params;
    _client = client;
    initialized = true;
    // Main loop
    while (true) {

        // update cached timing
        Timer::cacheElapsedSeconds();

        // Advance worker and client logic
        if (isWorker) worker->advance();
        if (isClient) client->advance();

        // Advance message queue and run callbacks for done messages
        MyMpi::getMessageQueue().advance();

        // Check termination
        if (distTerm.triggered())
            Terminator::setTerminating();
        if (allDone)
            Terminator::setTerminating();
        if (params.timeLimit() > 0 && Timer::elapsedSecondsCached() > params.timeLimit())
            Terminator::setTerminating();
        if (Terminator::isTerminating(true)) {
            distTerm.trigger(); // if not triggered already   
            break;
        }

        // Sleep and/or yield thread
        if (params.sleepMicrosecs() > 0) usleep(params.sleepMicrosecs());
        if (params.yield()) std::this_thread::yield();
    }

    // Clean up
    if (isWorker) delete worker;
    if (isClient) delete client;
  } catch (const std::exception& ex) {
      LOG(V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
      Process::doExit(1);
  } catch (...) {
      LOG(V0_CRIT, "[ERROR] uncaught exception\n");
      Process::doExit(1);
  }

  // Exit properly
  MyMpi::getMessageQueue().close();
  distTerm.reset();
  MPI_Barrier(MPI_COMM_WORLD);
  delete &MyMpi::getMessageQueue();
  if (clientComm != MPI_COMM_NULL) MPI_Comm_free(&clientComm);
  if (workerComm != MPI_COMM_NULL) MPI_Comm_free(&workerComm);
  MPI_Finalize();
  TmpDir::wipe();
  Process::removeDelayedExitWatchers();
  LOG(V2_INFO, "Exiting happily\n");

  terminated = true;
  return 0;
}


satcheck_mallobt::satcheck_mallobt(message_handlert &message_handler)
  : cnf_solvert(message_handler)
{
  // Initialize model and failed assumptions
  _model.clear();
  _formula.clear();

  if (!started) {

  static int argc = 4;
  static char* argv[] = {strdup("mallob"), strdup("-t=16"), strdup("-verbosity=4"), strdup("-compress-models"), nullptr};

  std::thread mallob_thread([&]() {
      main_mallob(argc, argv);
  });
  mallob_thread.detach();

  }
}

satcheck_mallobt::~satcheck_mallobt() { 
  assert(!pending && "Pending jobs should be finished before destruction");
  allDone = true;

  while (!terminated) {
    usleep(100000);
  }

  _model.clear();
  _failed_assumptions.clear();
  _formula.clear();

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
  assert(!pending && "Pending jobs should be finished before destruction");
  log.status() << "Entered prop solve" << messaget::eom;

  INVARIANT(status != statust::ERROR, "there cannot be an error");

  log.statistics() << (no_variables() - 1) << " variables, " << clause_counter
                   << " clauses" << messaget::eom;

  std::vector<int> currAssumptions;
  // Check for trivial UNSAT from assumptions
  for(const auto &a : assumptions)
  {
    if(a.is_false())
    {
      log.status() << "got FALSE as assumption: instance is UNSATISFIABLE"
                  << messaget::eom;
      status = statust::UNSAT;
      return resultt::P_UNSATISFIABLE;
    } else if (!a.is_true()) {
      currAssumptions.push_back(a.dimacs());
    }
  }

  // Wait for the initialization to finish
  while (!initialized) {
      usleep(100000);
  }

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

  pending = true;

  std::string precursor = job_id == 0 ? "" : "cbmc.cbmc-job-rev." + std::to_string(job_id-1);
    // Write a job JSON for the singular job to solve
    nlohmann::json json = {
      {"user", "cbmc"},
      {"name", "cbmc-job-rev." + std::to_string(job_id)},
      {"incremental", true},
      //{"precursor", precursor},
      //{"done", true},
      //{"files", {"/home/oguz/Desktop/hiwi_code/cbmc_mallob_monolithic/mallob/instances/r3unknown_10k.cnf"}},
      {"literals", _formula},
      {"assumptions", currAssumptions},
      {"priority", 1.000},
      {"application", "SAT"}
  };
  //if (precursor != "") json["precursor"] = precursor;
  // if (_params->crossJobCommunication()) json["group-id"] = "1";
  // if (_params->jobWallclockLimit() > 0)
  //     json["wallclock-limit"] = std::to_string(_params->jobWallclockLimit()) + "s";
  // if (_params->jobCpuLimit() > 0) {
  //     json["cpu-limit"] = std::to_string(_params->jobCpuLimit()) + "s";
  // }
 
  auto result = _client->getAPI().submit(json, [&](nlohmann::json& response) {
      job_id++;
      result_json = std::move(response);
      pending = false;
  });
  if (result != JsonInterface::Result::ACCEPT) {
      LOG(V0_CRIT, "[ERROR] Cannot introduce job!\n");
      abort();
  }

  while (pending) {
      usleep(100000);
  }
  
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
        log.status() << Timer::elapsedSeconds() << " Got model" << modelLits.size() << messaget::eom;
        for (int lit : modelLits) {
          const int var = std::abs(lit);
          _model[var] = lit;
        }
      } 
    } else if (j["result"]["solution"].is_string()) {
      // Handle single compressed model string
      std::string compressedModel = j["result"]["solution"].get<std::string>();
      log.status() << Timer::elapsedSeconds() << " Got compressed model" << compressedModel.c_str() << messaget::eom;
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
        
  // // DUMMY IMPLEMENTATION: Always return SAT
  // log.status() << "SAT checker (DUMMY): instance is SATISFIABLE" << messaget::eom;
  
  // status = statust::SAT;
  // return resultt::P_SATISFIABLE;
}
//#endif


