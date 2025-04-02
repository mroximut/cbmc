#include "satcheck_mallob.h"

#include <util/exception_utils.h>
#include <util/invariant.h>
#include <util/narrow.h>
#include <util/threeval.h>


#ifdef HAVE_MALLOB
#include "interface/api/api_connector.hpp"
#include "interface/api/api_registry.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#endif

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
  INVARIANT(status != statust::ERROR, "there cannot be an error");

  log.statistics() << (no_variables() - 1) << " variables, " << clause_counter
                   << " clauses" << messaget::eom;

  // if assumptions contains false, we need this to be UNSAT
  for(const auto &a : assumptions)
  {
    if(a.is_false())
    {
      log.status() << "got FALSE as assumption: instance is UNSATISFIABLE"
                   << messaget::eom;
      status = statust::UNSAT;
      return resultt::P_UNSATISFIABLE;
    }
  }

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