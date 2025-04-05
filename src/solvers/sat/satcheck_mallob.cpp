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

bool monoJobDone = false;
nlohmann::json result_json;

void introduceMonoJob(Parameters& params, Client& client) {

    // Write a job JSON for the singular job to solve
    nlohmann::json json = {
        {"user", "admin"},
        {"name", "mono-job"},
        //{"files", {params.monoFilename()}},
        {"literals", {1, 2, 3, 0}},
        {"priority", 1.000},
        {"application", "SAT"}
    };
    if (params.crossJobCommunication()) json["group-id"] = "1";
    if (params.jobWallclockLimit() > 0)
        json["wallclock-limit"] = std::to_string(params.jobWallclockLimit()) + "s";
    if (params.jobCpuLimit() > 0) {
        json["cpu-limit"] = std::to_string(params.jobCpuLimit()) + "s";
    }

    auto result = client.getAPI().submit(json, [&](nlohmann::json& response) {
        // Job done? => Terminate all processes
        monoJobDone = true;
        result_json = std::move(response);
    });
    if (result != JsonInterface::Result::ACCEPT) {
        LOG(V0_CRIT, "[ERROR] Cannot introduce mono job!\n");
        abort();
    }
}

inline bool doTerminate(Parameters& params, int rank) {
    
    bool terminate = false;
    if (Terminator::isTerminating(/*fromMainThread=*/true)) {
        terminate = true;
        MyMpi::broadcastExitSignal();
    }
    if (monoJobDone || (params.timeLimit() > 0 && Timer::elapsedSecondsCached() > params.timeLimit())) {
        terminate = true;
        MyMpi::broadcastExitSignal();
    }
    if (terminate) {
        if (rank == 0) {
            LOG(V2_INFO, "Terminating.\n");
        } else {
            LOG(V3_VERB, "Terminating.\n");
        }
        Terminator::setTerminating();
        return true;
    }
    return false;
}

void doMainProgram(MPI_Comm& commWorkers, MPI_Comm& commClients, Parameters& params, DistributedTermination& distTerm) {

    // Determine which role(s) this PE has
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

    // If job streaming is enabled, initialize a corresponding job streamer
    JobStreamer* streamer = nullptr;
    if (params.jobTemplate.isSet() && isClient) {
        streamer = new JobStreamer(params, client->getAPI(), client->getInternalRank());
    }
    
    // If mono solving mode is enabled, introduce the singular job to solve
    if (/*params.monoFilename.isSet() &&*/ isClient && MyMpi::rank(commClients) == 0)
        introduceMonoJob(params, *client);

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
        if (monoJobDone)
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
    if (streamer) delete streamer;
    if (isWorker) delete worker;
    if (isClient) delete client;
}


int main_mallob(int argc, char *argv[]) {
    
  MyMpi::init();
  Timer::init();
  Proc::nameThisThread("MainThread");

  int numNodes = MyMpi::size(MPI_COMM_WORLD);
  int rank = MyMpi::rank(MPI_COMM_WORLD);

  Parameters params;
  argc = 3;
  argv[1] = strdup("-t=16");
  argv[2] = strdup("-compress-models");
  argv[3] = strdup("-verbosity=6");
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
      doMainProgram(workerComm, clientComm, params, *distTerm.get());
  } catch (const std::exception& ex) {
      LOG(V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
      Process::doExit(1);
  } catch (...) {
      LOG(V0_CRIT, "[ERROR] uncaught exception\n");
      Process::doExit(1);
  }

  if (!result_json.empty()) {
      LOG(V2_INFO, "Result: %s\n", result_json.dump().c_str());
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

  return 0;
}


satcheck_mallobt::satcheck_mallobt(message_handlert &message_handler)
  : cnf_solvert(message_handler)
{
  // Initialize model and failed assumptions
  _model.clear();
  _failed_assumptions.clear();
}

satcheck_mallobt::~satcheck_mallobt() = default;

std::string satcheck_mallobt::solver_text() const
{
  return "Mallob SAT Solver";
}

tvt satcheck_mallobt::l_get(literalt a) const
{
  if(a.is_constant())
    return tvt(a.sign());

  // Check if the variable is in bounds
  if(a.var_no() >= _model.size())
    return tvt(tvt::tv_enumt::TV_UNKNOWN);

  const int val = _model[a.var_no()];
  if(val > 0)
    return tvt(true);
  else if(val < 0)
    return tvt(false);
  else
    return tvt(tvt::tv_enumt::TV_UNKNOWN);
}

void satcheck_mallobt::lcnf(const bvt &bv)
{
  for(const auto &lit : bv)
  {
    if(lit.is_true())
      return;
    else if(!lit.is_false())
      INVARIANT(lit.var_no() < no_variables(), "reject out of bound variables");
  }

  // In a real implementation, we would add the clause to Mallob
  // For this dummy implementation, we just count clauses

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
  return std::find(
           _failed_assumptions.begin(),
           _failed_assumptions.end(),
           a.dimacs()) != _failed_assumptions.end();
}

propt::resultt satcheck_mallobt::do_prop_solve(const bvt &assumptions)
{

  log.status() << "Entered prop solve" << messaget::eom;

  INVARIANT(status != statust::ERROR, "there cannot be an error");

  log.statistics() << (no_variables() - 1) << " variables, " << clause_counter
                   << " clauses" << messaget::eom;

  // Run mallob with a dummy mono job

  char* mallob_argv[] = {(char*)"mallob", nullptr};
  
  std::thread mallob_thread([&]() {
    int ret = main_mallob(1, mallob_argv);
  });

  mallob_thread.join(); // Wait for thread completion
  log.status() << "Mallob thread finished" << messaget::eom;  
  
 
  // // Check for trivial UNSAT from assumptions
  // for(const auto &a : assumptions)
  // {
  //   if(a.is_false())
  //   {
  //     log.status() << "got FALSE as assumption: instance is UNSATISFIABLE"
  //                 << messaget::eom;
  //     status = statust::UNSAT;
  //     return resultt::P_UNSATISFIABLE;
  //   }
  // }

  // // if assumptions contains false, we need this to be UNSAT
  // for(const auto &a : assumptions)
  // {
  //   if(a.is_false())
  //   {
  //     log.status() << "got FALSE as assumption: instance is UNSATISFIABLE"
  //                  << messaget::eom;
  //     status = statust::UNSAT;
  //     return resultt::P_UNSATISFIABLE;
  //   }
  // }

  // DUMMY IMPLEMENTATION: Always return SAT
  log.status() << "SAT checker (DUMMY): instance is SATISFIABLE" << messaget::eom;
  
  // Generate a model where all variables are set to true
  _model.resize(no_variables() + 1, 0);
  for(size_t i = 1; i <= no_variables(); i++)
  {
    _model[i] = i; // All variables are positive
  }
  
  status = statust::SAT;
  return resultt::P_SATISFIABLE;
}
//#endif

