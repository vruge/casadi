/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "sqp_internal.hpp"
#include "symbolic/stl_vector_tools.hpp"
#include "symbolic/matrix/sparsity_tools.hpp"
#include "symbolic/matrix/matrix_tools.hpp"
#include "symbolic/fx/sx_function.hpp"
#include "symbolic/sx/sx_tools.hpp"
#include "symbolic/casadi_calculus.hpp"
#include <ctime>
#include <iomanip>
#include <fstream>
#include <cmath>
#include <cfloat>
#include <deque>

using namespace std;
namespace CasADi{

SQPInternal::SQPInternal(const FX& F, const FX& G, const FX& H, const FX& J) : NLPSolverInternal(F,G,H,J){
  casadi_warning("The SQP method is under development");
  addOption("qp_solver",         OT_QPSOLVER,   GenericType(),    "The QP solver to be used by the SQP method");
  addOption("qp_solver_options", OT_DICTIONARY, GenericType(),    "Options to be passed to the QP solver");
  addOption("hessian_approximation", OT_STRING, "limited-memory", "limited-memory|exact");
  addOption("maxiter",           OT_INTEGER,      50,             "Maximum number of SQP iterations");
  addOption("maxiter_ls",        OT_INTEGER,       3,             "Maximum number of linesearch iterations");
  addOption("tol_pr",            OT_REAL,       1e-6,             "Stopping criterion for primal infeasibility");
  addOption("tol_du",            OT_REAL,       1e-6,             "Stopping criterion for dual infeasability");
  addOption("c1",                OT_REAL,       1E-4,             "Armijo condition, coefficient of decrease in merit");
  addOption("beta",              OT_REAL,       0.8,              "Line-search parameter, restoration factor of stepsize");
  addOption("merit_memory",      OT_INTEGER,      4,              "Size of memory to store history of merit function values");
  addOption("lbfgs_memory",      OT_INTEGER,     10,              "Size of L-BFGS memory.");
  addOption("regularize",        OT_BOOLEAN,      0,              "Automatic regularization of Lagrange Hessian.");
  
  // Monitors
  addOption("monitor",      OT_STRINGVECTOR, GenericType(),  "", "eval_f|eval_g|eval_jac_g|eval_grad_f|eval_h|qp|dx", true);
}


SQPInternal::~SQPInternal(){
}

void SQPInternal::init(){
  // Call the init method of the base class
  NLPSolverInternal::init();
    
  // Read options
  maxiter_ = getOption("maxiter");
  maxiter_ls_ = getOption("maxiter_ls");
  c1_ = getOption("c1");
  beta_ = getOption("beta");
  merit_memsize_ = getOption("merit_memory");
  lbfgs_memory_ = getOption("lbfgs_memory");
  tol_pr_ = getOption("tol_pr");
  tol_du_ = getOption("tol_du");
  
  if (getOption("hessian_approximation")=="exact" && H_.isNull()) {
    if (!getOption("generate_hessian")){
      casadi_error("SQPInternal::evaluate: you set option 'hessian_approximation' to 'exact', but no hessian was supplied. Try with option \"generate_hessian\".");
    }
  }
  
  // If the Hessian is generated, we use exact approximation by default
  if (bool(getOption("generate_hessian"))){
    setOption("hessian_approximation", "exact");
  }
  
  // Allocate a QP solver
  CRSSparsity H_sparsity = getOption("hessian_approximation")=="exact"? H_.output().sparsity() : sp_dense(n_,n_);
  H_sparsity = H_sparsity + DMatrix::eye(n_).sparsity();
  CRSSparsity A_sparsity = J_.isNull() ? CRSSparsity(0,n_,false) : J_.output().sparsity();

  QPSolverCreator qp_solver_creator = getOption("qp_solver");
  qp_solver_ = qp_solver_creator(H_sparsity,A_sparsity);

  // Set options if provided
  if(hasSetOption("qp_solver_options")){
    Dictionary qp_solver_options = getOption("qp_solver_options");
    qp_solver_.setOption(qp_solver_options);
  }
  qp_solver_.init();
  
  // Lagrange multipliers of the NLP
  mu_.resize(m_);
  mu_x_.resize(n_);
  
  // Lagrange gradient in the next iterate
  gLag_.resize(n_);

  // Current linearization point, default: initial guess
  x_.resize(n_);

  // Previous linearization point
  x_old_.resize(n_);

  // Candidate
  x_cand_.resize(n_);
}

void SQPInternal::evaluate(int nfdir, int nadir){
  casadi_assert(nfdir==0 && nadir==0);
  
  checkInitialBounds();
  
  // Get problem data
  const vector<double>& x_init = input(NLP_X_INIT).data();
  const vector<double>& lbx = input(NLP_LBX).data();
  const vector<double>& ubx = input(NLP_UBX).data();
  const vector<double>& lbg = input(NLP_LBG).data();
  const vector<double>& ubg = input(NLP_UBG).data();
  
  // Set the static parameter
  if (parametric_) {
    if (!F_.isNull()) F_.setInput(input(NLP_P),F_.getNumInputs()-1);
    if (!G_.isNull()) G_.setInput(input(NLP_P),G_.getNumInputs()-1);
    if (!H_.isNull()) H_.setInput(input(NLP_P),H_.getNumInputs()-1);
    if (!J_.isNull()) J_.setInput(input(NLP_P),J_.getNumInputs()-1);
  }
    
  // Current linearization point, default: initial guess
  copy(x_init.begin(),x_init.end(),x_.begin());
  
  // Actual correction
  DMatrix p;
  // Initial guess for Lagrange Hessian
  DMatrix B0 = DMatrix::eye(n_);
  // Storage for Lagrange Hessian
  DMatrix Bk;

  // Cost function value
  double fk;
  // Constraint function value
  DMatrix gk;
  // Constraint Jacobian
  DMatrix Jgk;
  // Lagrange multipliers of the NLP
  fill(mu_.begin(),mu_.end(),0);
  fill(mu_x_.begin(),mu_x_.end(),0);

  // Lagrange gradient in the next iterate
  fill(gLag_.begin(),gLag_.end(),0);

  // Initial Hessian approximation of BFGS
  if ( getOption("hessian_approximation") == "limited-memory") {
    Bk = DMatrix::eye(n_);
    makeDense(Bk);
  }

  if (monitored("eval_h")) {
    cout << "(pre) B = " << endl;
    Bk.printSparse();
  }
    
  double inf = numeric_limits<double>::infinity();
  qp_solver_.input(QP_LBX).setAll(-inf);
  qp_solver_.input(QP_UBX).setAll( inf);

  // Storage for merit function
  std::deque<double> merit_mem;

  // Printing header
  stringstream header;
  header << "   It.     ";
  header << "obj           ";
  header << "pr_inf        "; 
  header << "du_inf        ";
  header << "corr_norm    ";
  header << "stepsize     ";
  header << "ls-trials    " << endl;
  cout << header.str();
  int it_counter = 1;

  sigma_ = 0.;

  // MAIN OPTIMIZATION LOOP
  while(true){
    // Printing header occasionally
    if (it_counter % 10 == 0) cout << header.str();
    // Evaluating Hessian if needed
    if (getOption("hessian_approximation") == "exact") {
      int n_hess_in = H_.getNumInputs() - (parametric_ ? 1 : 0);
      H_.setInput(x_);
      if(n_hess_in>1){
        H_.setInput(mu_, n_hess_in == 4 ? 2 : 1);
        H_.setInput(1, n_hess_in == 4 ? 3 : 2);
      }
      H_.evaluate();
      Bk = H_.output();
      // Determing regularization parameter with Gershgorin theorem
      if (bool(getOption("regularize"))){
        vector<int> rowind,col;
        Bk.sparsity().getSparsityCRS(rowind, col);
        std::vector<double>& data = Bk.data();
        double radius;
        double reg_param = 0;
        double mineig;
        for(int i=0; i<rowind.size()-1; ++i){
          radius = 0;
          for(int el=rowind[i]; el<rowind[i+1]; ++el){
            int j = col[el];
            if(i != j){
              radius += fabs(data[el]);
            }
  //          cout << "(" << r << "," << col[el] << "): " << data[el] << endl; 
          }
          mineig = Bk(i, i).elem(0) - radius;
          if (mineig < reg_param){
            reg_param = mineig;
          }
        }
  //      cout << "Regularization parameter: " << -reg_param << endl;
        if ( reg_param < 0.){
          Bk += -reg_param * DMatrix::eye(Bk.size1());
        }
        else{
          Bk += 0. * DMatrix::eye(Bk.size1());
        }
      }
    }
    if (monitored("eval_h")) {
      cout << "(main loop) B = " << endl;
      Bk.printSparse();
    }
    // Use identity Hessian
    //Bk = DMatrix::eye(Bk.size1());

    if(m_>0){
      // Evaluate the constraint function
      G_.setInput(x_);
      G_.evaluate();
      gk = G_.output();
      
      if (monitored("eval_g")) {
        cout << "(main loop) x = " << x_ << endl;
        cout << "(main loop) G = " << endl;
        G_.output().printSparse();
      }
      
      // Evaluate the constraint Jacobian
      J_.setInput(x_);
      J_.evaluate();
      Jgk = J_.output();

      if (monitored("eval_jac_g")) {
        cout << "(main loop) x = " << x_ << endl;
        cout << "(main loop) J = " << endl;
        J_.output().printSparse();
      }
    }
    
    // Evaluate the gradient of the objective function
    F_.setInput(x_);
    F_.setAdjSeed(1.0);
    F_.evaluate(0,1);
    F_.getOutput(fk);
    
    // Gradient of objective
    const DMatrix& gfk = F_.adjSens();
    
    if (monitored("eval_f")){
      cout << "(main loop) x = " << x_ << endl;
      cout << "(main loop) F = " << endl;
      F_.output().printSparse();
    }
    
    if (monitored("eval_grad_f")) {
      cout << "(main loop) x = " << x_ << endl;
      cout << "(main loop) gradF = " << endl;
      gfk.printSparse();
    }
    
    // Pass data to QP solver
    qp_solver_.setInput(Bk, QP_H);
    qp_solver_.setInput(gfk,QP_G);
    // Hot-starting if possible
    if (p.size1()){
      qp_solver_.setInput(p, QP_X_INIT);
      //TODO: Fix hot-starting of dual variables
      //qp_solver_.setInput(mu_qp, QP_LAMBDA_INIT);
    }
      
    if(m_>0){
      qp_solver_.setInput(Jgk,QP_A);
      qp_solver_.setInput(input(NLP_LBG)-gk,QP_LBA);
      qp_solver_.setInput(input(NLP_UBG)-gk,QP_UBA);
    }

    transform(lbx.begin(),lbx.end(),x_.begin(),qp_solver_.input(QP_LBX).begin(),minus<double>());
    transform(ubx.begin(),ubx.end(),x_.begin(),qp_solver_.input(QP_UBX).begin(),minus<double>());
    
    if (monitored("qp")) {
      cout << "(main loop) QP_H = " << endl;
      qp_solver_.input(QP_H).printDense();
      cout << "(main loop) QP_A = " << endl;
      qp_solver_.input(QP_A).printDense();
      cout << "(main loop) QP_G = " << endl;
      qp_solver_.input(QP_G).printDense();
      cout << "(main loop) QP_LBA = " << endl;
      qp_solver_.input(QP_LBA).printDense();
      cout << "(main loop) QP_UBA = " << endl;
      qp_solver_.input(QP_UBA).printDense();
      cout << "(main loop) QP_LBX = " << endl;
      qp_solver_.input(QP_LBX).printDense();
      cout << "(main loop) QP_UBX = " << endl;
      qp_solver_.input(QP_UBX).printDense();
    }

    // Solve the QP subproblem
    qp_solver_.evaluate();

    // Get the optimal solution
    p = qp_solver_.output(QP_PRIMAL);
    if (monitored("dx")){
      cout << "(main loop) dx = " << endl;
      cout << p << endl;
    }
    // Detecting indefiniteness
//    if ((norm_2(p) / norm_2(x)).at(0) > 500.){
//      casadi_warning("Search direction has very large values, indefinite Hessian might have ouccured.");
//    }
    double gain = inner_prod(p, mul(Bk, p)).at(0);
    if (gain < 0){
      casadi_warning("Indefinite Hessian detected...");
    }
    
    
    // Get the dual solution for the inequalities
    const vector<double>& mu_qp = qp_solver_.output(QP_LAMBDA_A).data();
    const vector<double>& mu_x_qp = qp_solver_.output(QP_LAMBDA_X).data();

    // Calculate penalty parameter of merit function
    for(int j=0; j<m_; ++j){
      if( fabs(mu_qp[j]) > sigma_){
        sigma_ = fabs(mu_qp[j]) * 1.01;
      }
    }
//    for(int j = 0; j < n; ++j){
//      if( fabs(mu_x_qp[j]) > sigma_){
//        sigma_ = fabs(mu_x_qp[j]) * 1.01;
//      }
//    }

    // Calculate L1-merit function in the actual iterate
    double l1_infeas = 0.;
    for(int j=0; j<m_; ++j){
      // Left-hand side violated
      if(lbg[j] - gk.elem(j) > 0.){
        l1_infeas += lbg[j] - gk.elem(j);
      }
      else if (gk.elem(j) - ubg[j] > 0.){
        l1_infeas += gk.elem(j) - ubg[j];
      }
    }

    // Right-hand side of Armijo condition
    F_.setFwdSeed(p);
    F_.evaluate(1, 0);

    double L1dir = F_.fwdSens().elem(0) - sigma_ * l1_infeas;
    double L1merit = fk + sigma_ * l1_infeas;

    // Storing the actual merit function value in a list
    merit_mem.push_back(L1merit);
    if (merit_mem.size() > merit_memsize_){
      merit_mem.pop_front();
    }

    // Default stepsize
    double t = 1.0;   
    DMatrix gk_cand;
    double fk_cand;
    // Merit function value in candidate
    double L1merit_cand = 0.;

    // Line-search loop
    int ls_counter = 1;
    while (true){
      for(int i=0; i<n_; ++i) x_cand_[i] = x_[i] + t * p.at(i); 
      // Evaluating objective and constraints
      F_.setInput(x_cand_);
      F_.evaluate();
      F_.getOutput(fk_cand);
      l1_infeas = 0.;
      if (!G_.isNull()){
        G_.setInput(x_cand_);
        G_.evaluate();  
        gk_cand = G_.output();

        // Calculating merit-function in candidate
        for(int j=0; j<m_; ++j){
          // Left-hand side violated
          if (lbg[j] - gk_cand.elem(j) > 0.){
            l1_infeas += lbg[j] - gk_cand.elem(j);
          }
          else if (gk_cand.elem(j) - ubg[j] > 0.){
            l1_infeas += gk_cand.elem(j) - ubg[j];
          }
        }
      }
      L1merit_cand = fk_cand + sigma_ * l1_infeas;
      // Calculating maximal merit function value so far
      double meritmax = -1E20;
      for(int k = 0; k < merit_mem.size(); ++k){
        if (merit_mem[k] > meritmax){
          meritmax = merit_mem[k];
        }
      }
      if (L1merit_cand <= meritmax + t * c1_ * L1dir){ 
        // Accepting candidate
        break;
      }
      else{
        // Backtracking
        t = beta_ * t; 
      }

      // Line-search not successful, but we accept it.
      if(ls_counter == maxiter_ls_){
        break;
      }
      ++ls_counter;
    }
    // Candidate accepted
    copy(x_.begin(),x_.end(),x_old_.begin());
    copy(x_cand_.begin(),x_cand_.end(),x_.begin());
    fk = fk_cand;
    gk = gk_cand;
    for(int i=0; i<m_; ++i) mu_[i] = t * mu_qp[i] + (1 - t) * mu_[i];
    for(int i=0; i<n_; ++i) mu_x_[i] = t * mu_x_qp[i] + (1 - t) * mu_x_[i];

    // Evaluating objective gradient
    F_.setInput(x_);
    F_.setAdjSeed(1.0);
    F_.evaluate(0, 1);
    F_.getAdjSens(gLag_); 

    // Adjoint derivative of constraint function
    if(m_>0){
      G_.setAdjSeed(mu_);
      G_.evaluate(0, 1);
      transform(gLag_.begin(),gLag_.end(),G_.adjSens().begin(),gLag_.begin(),plus<double>()); // gLag_ += G_.adjSens()
    }
    transform(gLag_.begin(),gLag_.end(),mu_x_.begin(),gLag_.begin(),plus<double>()); // gLag_ += mu_x_;

    DMatrix gLag_old_bfgs;
    F_.setInput(x_old_);
    F_.setAdjSeed(1.0);
    F_.evaluate(0, 1);
    gLag_old_bfgs = F_.adjSens();
    if (!G_.isNull()){
      G_.setInput(x_old_);
      G_.setAdjSeed(mu_);
      G_.evaluate(0, 1);
      gLag_old_bfgs += G_.adjSens();
    }
    gLag_old_bfgs += mu_x_;

    // Updating Lagrange Hessian if needed. (BFGS with careful updates and restarts)
    if (getOption("hessian_approximation") == "limited-memory") { 
      if (it_counter % lbfgs_memory_ == 0){
        Bk = diag(diag(Bk));
      }
      DMatrix sk = DMatrix(x_) - DMatrix(x_old_);
      DMatrix yk = gLag_ - gLag_old_bfgs;
      DMatrix qk = mul(Bk, sk);
      // Calculating theta
      double omega = 1.;
      if (inner_prod(yk, sk).at(0) < 0.2 * inner_prod(sk, qk).at(0) ){
        double skBksk = inner_prod(sk, qk).at(0);
        omega = 0.8 * skBksk / (skBksk - inner_prod(sk, yk).at(0)); 
      }
      yk = omega * yk + (1 - omega) * qk;

      double theta = 1. / inner_prod(sk, yk).at(0);
      double phi = 1. / inner_prod(qk, sk).at(0);
      Bk = Bk + theta * mul(yk, yk.trans()) - phi * mul(qk, qk.trans());

    }
    // Calculating optimality criterion
    // Primal infeasability
    double pr_infG = 0.;
    double pr_infx = 0.;
    if (!G_.isNull()){
      // Nonlinear constraints
      for(int j=0; j<m_; ++j){
        // Equality
        if (ubg[j] - lbg[j] < 1E-20){
          pr_infG += fabs(gk_cand.elem(j) - lbg[j]);
        }
        // Inequality, left-hand side violated
        else if(lbg[j] - gk_cand.elem(j) > 0.){
          pr_infG += lbg[j] - gk_cand.elem(j);
        }
        // Inequality, right-hand side violated
        else if(gk_cand.elem(j) - ubg[j] > 0.){
          pr_infG += gk_cand.elem(j) - ubg[j];
          //cout << color << "SQP: " << mu.elem(j) << defcol << endl;
        }
      }
      // Bound constraints
      for(int j=0; j<n_; ++j){
        // Equality
        if (ubx[j] - lbx[j] < 1E-20){
          pr_infx += fabs(x_[j] - lbx[j]);
        }
        // Inequality, left-hand side violated
        else if ( lbx[j] - x_[j] > 0.){
          pr_infx += lbx[j] - x_[j];
        }
        // Inequality, right-hand side violated
        else if ( x_[j] - ubx[j] > 0.){
          pr_infx += x_[j] - ubx[j];
        }
      }
    }
    double pr_inf = pr_infG + pr_infx;
    
    // 1-norm of lagrange gradient
    double gLag_norm1 = 0;
    for(vector<double>::const_iterator it=gLag_.begin(); it!=gLag_.end(); ++it) gLag_norm1 += fabs(*it);
    
    // Printing information about the actual iterate
    cout << setprecision(3);
    cout << "  ";
    cout << setw(3);
    cout << it_counter            << "     ";
    cout << scientific;
    cout << fk_cand               << "     ";
    cout << pr_inf                << "     ";
    cout << gLag_norm1            << "     ";
    cout << norm_1(p).elem(0)     << "     ";
    cout << t                     << "     ";
    char ls_success = (ls_counter == maxiter_ls_) ? 'F' :  ' ';
       
    cout << ls_counter << ls_success << "    "; 
    cout << endl;
    
    // Call callback function if present
    if (!callback_.isNull()) {
      callback_.input(NLP_X_OPT).set(fk);
      callback_.input(NLP_COST).set(fk);
      callback_.input(NLP_X_OPT).set(x_);
      callback_.input(NLP_LAMBDA_G).set(mu_);
      callback_.input(NLP_LAMBDA_X).set(mu_x_);
      callback_.input(NLP_G).set(gk);
      callback_.evaluate();
      
      if (callback_.output(0).at(0)) {
       cout << "SQP: aborted by callback...\n"; 
       break;
      }
    }

    // Checking convergence criteria
    if (pr_inf < tol_pr_ && gLag_norm1 < tol_du_){
      cout << "SQP: Convergence achieved after " << it_counter << " iterations.\n";
      break;
    }

    if (it_counter == maxiter_){
      cout << "SQP: Maximum number of iterations reached, quiting...\n"; 
      break;
    }
    ++it_counter;
  }
  
  output(NLP_COST).set(fk);
  output(NLP_X_OPT).set(x_);
  output(NLP_LAMBDA_G).set(mu_);
  output(NLP_LAMBDA_X).set(mu_x_);
  output(NLP_G).set(gk);
  
  // Save statistics
  stats_["iter_count"] = it_counter;
}

} // namespace CasADi
