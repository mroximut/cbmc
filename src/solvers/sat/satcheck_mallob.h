#ifndef CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H
#define CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H

#include <solvers/sat/cnf.h>
#include <set>
#include <vector>
#include <solvers/hardness_collector.h>

//#ifdef HAVE_MALLOB
class CBMCSatConnector;
//#endif

class satcheck_mallobt : public cnf_solvert, public hardness_collectort
{
  static int streamIdCounter;

public:
  satcheck_mallobt(message_handlert &message_handler); 
  virtual ~satcheck_mallobt() override; 

  std::string solver_text() const override;
  tvt l_get(literalt a) const override; // retrieve the value of a literal

  void lcnf(const bvt &bv) override; // add a clause to the formula
  void set_assignment(literalt a, bool value) override; // force a literal to have a specific value

  bool has_assumptions() const override // support for assumptions
  {
    return true;
  }
  
  bool has_is_in_conflict() const override  // support for conflict identification
  {
    return true;
  }
  
  bool is_in_conflict(literalt a) const override; // check if a literal is part of the conflict

protected:
  resultt do_prop_solve(const bvt &assumptions) override; // solve the SAT problem with given assumptions

private:
  std::vector<int> _model;
  std::set<int> _failed_assumptions;
  std::vector<int> _formula;
  bool _empty_clause = false;
  
  //#ifdef HAVE_MALLOB
  CBMCSatConnector* _sat_connector {nullptr};
  //#endif
};

#endif // CPROVER_SOLVERS_SAT_SATCHECK_MALLOB_H