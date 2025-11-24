#ifndef CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H
#define CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H

#include "cnf.h"
#include <set>
#include <vector>
#include <solvers/hardness_collector.h>

//#ifdef HAVE_MALLOB
// class APIConnector;
// class SatJobStream;
class CBMCSatSolver;
//#endif

#include <functional>

class satcheck_mallobt : public cnf_solvert, public hardness_collectort
{
  //static int streamIdCounter;
  //static APIConnector* _api;

public:
  satcheck_mallobt(message_handlert &message_handler); 
  virtual ~satcheck_mallobt() override; 

  const std::string solver_text() override;
  tvt l_get(literalt a) const override final; // retrieve the value of a literal

  void lcnf(const bvt &bv) override final; // add a clause to the formula
  void set_assignment(literalt a, bool value) override; // force a literal to have a specific value

  void set_assumptions(const bvt &_assumptions) override; // set assumptions for the SAT solver

  bool has_set_assumptions() const override final // support for assumptions
  {
    return true;
  }
  
  bool has_is_in_conflict() const override final  // support for conflict identification
  {
    return true;
  }
  
  bool is_in_conflict(literalt a) const override; // check if a literal is part of the conflict

  // factory function (can be assigned a lambda) that creates a CBMCSatSolver*
  static std::function<CBMCSatSolver*()> createCBMCSatSolver;

protected:
  resultt do_prop_solve() override; // solve the SAT problem with given assumptions

  bvt assumptions;

private:
  std::vector<int> _model;
  std::set<int> _failed_assumptions;
  std::vector<int> _formula;
  bool _empty_clause = false;
  
  //#ifdef HAVE_MALLOB
  //SatJobStream* _streamer;
  CBMCSatSolver* _sat_connector {nullptr};
  //#endif
};

#endif // CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H