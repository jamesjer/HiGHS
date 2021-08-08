/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2021 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/*    Authors: Julian Hall, Ivet Galabova, Qi Huangfu, Leona Gottwald    */
/*    and Michael Feldmeier                                              */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/**@file lp_data/HSimplex.cpp
 * @brief
 */

#include "simplex/HSimplex.h"

//#include "HConfig.h"
#include "lp_data/HighsLpUtils.h"
#include "util/HighsSort.h"

using std::runtime_error;
#include <cassert>
#include <vector>

#ifdef OPENMP
#include "omp.h"
#endif

void getSimplexScaling(HighsOptions& options, HighsLp& lp,
                       SimplexScale& scale) {
  // Possibly scale the LP
  bool scale_lp = options.simplex_scale_strategy != kSimplexScaleStrategyOff &&
                  lp.num_col_ > 0;
  const bool force_no_scaling = false;  // true;//
  if (force_no_scaling) {
    highsLogDev(options.log_options, HighsLogType::kWarning,
                "Forcing no scaling\n");
    scale_lp = false;
  }
  const bool analyse_lp_data =
      kHighsAnalysisLevelModelData & options.highs_analysis_level;
  if (analyse_lp_data) analyseLp(options.log_options, lp, "Unscaled");
  // Possibly scale the LP. At least set the scaling factors to 1
  if (scale_lp) {
    scaleSimplexLp(options, lp, scale);
    if (analyse_lp_data) analyseScaledLp(options.log_options, scale, lp);
  } else {
    // Initialise unit scaling factors
    initialiseScale(lp, scale);
  }
}

void scaleAndPassLpToEkk(HighsModelObject& highs_model_object) {
  HEkk& ekk_instance = highs_model_object.ekk_instance_;
  HighsOptions& options = highs_model_object.options_;
  HighsLp& lp = highs_model_object.lp_;
  // Possibly scale the LP
  bool scale_lp = options.simplex_scale_strategy != kSimplexScaleStrategyOff &&
                  highs_model_object.lp_.num_col_ > 0;
  const bool force_no_scaling = false;  // true;//
  if (force_no_scaling) {
    highsLogDev(options.log_options, HighsLogType::kWarning,
                "Forcing no scaling\n");
    scale_lp = false;
  }
  const bool analyse_lp_data =
      kHighsAnalysisLevelModelData & options.highs_analysis_level;
  if (analyse_lp_data) analyseLp(options.log_options, lp, "Unscaled");
  // Possibly scale the LP. At least set the scaling factors to 1
  SimplexScale& scale = highs_model_object.scale_;
  if (scale_lp) {
    HighsLp scaled_lp = lp;
    // Perform scaling - if it's worth it.
    scaleSimplexLp(options, scaled_lp, scale);
    if (analyse_lp_data) analyseScaledLp(options.log_options, scale, scaled_lp);
    // Pass the scaled LP to Ekk
    ekk_instance.passNewLp(scaled_lp);
  } else {
    // Initialise unit scaling factors
    initialiseScale(lp, scale);
    // Pass the original LP to Ekk
    ekk_instance.passNewLp(lp);
  }
}

void appendNonbasicColsToBasis(HighsLp& lp, HighsBasis& highs_basis,
                               HighsInt XnumNewCol) {
  assert(highs_basis.valid);
  if (!highs_basis.valid) {
    printf("\n!!Appending columns to invalid basis!!\n\n");
  }
  // Add nonbasic structurals
  if (XnumNewCol == 0) return;
  HighsInt newNumCol = lp.num_col_ + XnumNewCol;
  highs_basis.col_status.resize(newNumCol);
  // Make any new columns nonbasic
  for (HighsInt iCol = lp.num_col_; iCol < newNumCol; iCol++) {
    if (!highs_isInfinity(-lp.col_lower_[iCol])) {
      highs_basis.col_status[iCol] = HighsBasisStatus::kLower;
    } else if (!highs_isInfinity(lp.col_upper_[iCol])) {
      highs_basis.col_status[iCol] = HighsBasisStatus::kUpper;
    } else {
      highs_basis.col_status[iCol] = HighsBasisStatus::kZero;
    }
  }
}

void appendNonbasicColsToBasis(HighsLp& lp, SimplexBasis& basis,
                               HighsInt XnumNewCol) {
  // Add nonbasic structurals
  if (XnumNewCol == 0) return;
  HighsInt newNumCol = lp.num_col_ + XnumNewCol;
  HighsInt newNumTot = newNumCol + lp.num_row_;
  basis.nonbasicFlag_.resize(newNumTot);
  basis.nonbasicMove_.resize(newNumTot);
  // Shift the row data in basicIndex, nonbasicFlag and nonbasicMove if
  // necessary
  for (HighsInt iRow = lp.num_row_ - 1; iRow >= 0; iRow--) {
    HighsInt iCol = basis.basicIndex_[iRow];
    if (iCol >= lp.num_col_) {
      // This basic variable is a row, so shift its index
      basis.basicIndex_[iRow] += XnumNewCol;
    }
    basis.nonbasicFlag_[newNumCol + iRow] =
        basis.nonbasicFlag_[lp.num_col_ + iRow];
    basis.nonbasicMove_[newNumCol + iRow] =
        basis.nonbasicMove_[lp.num_col_ + iRow];
  }
  // Make any new columns nonbasic
  for (HighsInt iCol = lp.num_col_; iCol < newNumCol; iCol++) {
    basis.nonbasicFlag_[iCol] = kNonbasicFlagTrue;
    double lower = lp.col_lower_[iCol];
    double upper = lp.col_upper_[iCol];
    HighsInt move = kIllegalMoveValue;
    if (lower == upper) {
      // Fixed
      move = kNonbasicMoveZe;
    } else if (!highs_isInfinity(-lower)) {
      // Finite lower bound so boxed or lower
      if (!highs_isInfinity(upper)) {
        // Finite upper bound so boxed
        if (fabs(lower) < fabs(upper)) {
          move = kNonbasicMoveUp;
        } else {
          move = kNonbasicMoveDn;
        }
      } else {
        // Lower (since upper bound is infinite)
        move = kNonbasicMoveUp;
      }
    } else if (!highs_isInfinity(upper)) {
      // Upper
      move = kNonbasicMoveDn;
    } else {
      // FREE
      move = kNonbasicMoveZe;
    }
    assert(move != kIllegalMoveValue);
    basis.nonbasicMove_[iCol] = move;
  }
}

void appendBasicRowsToBasis(HighsLp& lp, HighsBasis& highs_basis,
                            HighsInt XnumNewRow) {
  assert(highs_basis.valid);
  if (!highs_basis.valid) {
    printf("\n!!Appending columns to invalid basis!!\n\n");
  }
  // Add basic logicals
  if (XnumNewRow == 0) return;
  HighsInt newNumRow = lp.num_row_ + XnumNewRow;
  highs_basis.row_status.resize(newNumRow);
  // Make the new rows basic
  for (HighsInt iRow = lp.num_row_; iRow < newNumRow; iRow++) {
    highs_basis.row_status[iRow] = HighsBasisStatus::kBasic;
  }
}

void appendBasicRowsToBasis(HighsLp& lp, SimplexBasis& basis,
                            HighsInt XnumNewRow) {
  // Add basic logicals
  if (XnumNewRow == 0) return;

  HighsInt newNumRow = lp.num_row_ + XnumNewRow;
  HighsInt newNumTot = lp.num_col_ + newNumRow;
  basis.nonbasicFlag_.resize(newNumTot);
  basis.nonbasicMove_.resize(newNumTot);
  basis.basicIndex_.resize(newNumRow);
  // Make the new rows basic
  for (HighsInt iRow = lp.num_row_; iRow < newNumRow; iRow++) {
    basis.nonbasicFlag_[lp.num_col_ + iRow] = kNonbasicFlagFalse;
    basis.nonbasicMove_[lp.num_col_ + iRow] = 0;
    basis.basicIndex_[iRow] = lp.num_col_ + iRow;
  }
}

void invalidateSimplexLpBasisArtifacts(HighsSimplexStatus& status) {
  // Invalidate the artifacts of the basis of the simplex LP
  status.has_matrix = false;
  // has_factor_arrays shouldn't be set false unless model dimension
  // changes, but invalidateSimplexLpBasisArtifacts is all that's
  // called when rows or columns are added, so can't change this now.
  status.has_factor_arrays = false;
  status.has_dual_steepest_edge_weights = false;
  status.has_nonbasic_dual_values = false;
  status.has_basic_primal_values = false;
  status.has_invert = false;
  status.has_fresh_invert = false;
  status.has_fresh_rebuild = false;
  status.has_dual_objective_value = false;
  status.has_primal_objective_value = false;
  status.has_dual_ray = false;
  status.has_primal_ray = false;
}

void invalidateSimplexLpBasis(HighsSimplexStatus& status) {
  // Invalidate the basis of the simplex LP, and all its other
  // properties - since they are basis-related
  status.has_basis = false;
  invalidateSimplexLpBasisArtifacts(status);
}

void invalidateSimplexLp(HighsSimplexStatus& status) {
  status.initialised = false;
  status.valid = false;
  invalidateSimplexLpBasis(status);
}

void updateSimplexLpStatus(HighsSimplexStatus& status, LpAction action) {
  switch (action) {
    case LpAction::kScale:
#ifdef HIGHSDEV
      printf(" LpAction::kScale\n");
#endif
      invalidateSimplexLpBasis(status);
      break;
    case LpAction::kNewCosts:
#ifdef HIGHSDEV
      printf(" LpAction::kNewCosts\n");
#endif
      status.has_nonbasic_dual_values = false;
      status.has_fresh_rebuild = false;
      status.has_dual_objective_value = false;
      status.has_primal_objective_value = false;
      break;
    case LpAction::kNewBounds:
#ifdef HIGHSDEV
      printf(" LpAction::kNewBounds\n");
#endif
      status.has_basic_primal_values = false;
      status.has_fresh_rebuild = false;
      status.has_dual_objective_value = false;
      status.has_primal_objective_value = false;
      break;
    case LpAction::kNewBasis:
#ifdef HIGHSDEV
      printf(" LpAction::kNewBasis\n");
#endif
      invalidateSimplexLpBasis(status);
      break;
    case LpAction::kNewCols:
#ifdef HIGHSDEV
      printf(" LpAction::kNewCols\n");
#endif
      invalidateSimplexLpBasisArtifacts(status);
      break;
    case LpAction::kNewRows:
#ifdef HIGHSDEV
      printf(" LpAction::kNewRows\n");
#endif
      invalidateSimplexLpBasisArtifacts(status);
      break;
    case LpAction::kDelCols:
#ifdef HIGHSDEV
      printf(" LpAction::kDelCols\n");
#endif
      invalidateSimplexLpBasis(status);
      break;
    case LpAction::kDelRows:
#ifdef HIGHSDEV
      printf(" LpAction::kDelRows\n");
#endif
      invalidateSimplexLpBasis(status);
      break;
    case LpAction::kDelRowsBasisOk:
#ifdef HIGHSDEV
      printf(" LpAction::kDelRowsBasisOk\n");
#endif
      //      info.lp_ = true;
      break;
    case LpAction::kScaledCol:
#ifdef HIGHSDEV
      printf(" LpAction::kScaledCol\n");
#endif
      invalidateSimplexLpBasisArtifacts(status);
      break;
    case LpAction::kScaledRow:
#ifdef HIGHSDEV
      printf(" LpAction::kScaledRow\n");
#endif
      invalidateSimplexLpBasisArtifacts(status);
      break;
    case LpAction::kBacktracking:
#ifdef HIGHSDEV
      printf(" LpAction::kBacktracking\n");
#endif
      status.has_matrix = false;
      status.has_nonbasic_dual_values = false;
      status.has_basic_primal_values = false;
      status.has_fresh_rebuild = false;
      status.has_dual_objective_value = false;
      status.has_primal_objective_value = false;
      break;
    default:
#ifdef HIGHSDEV
      printf(" Unrecognised LpAction::%" HIGHSINT_FORMAT "\n",
             (HighsInt)action);
#endif
      break;
  }
}

void unscaleSolution(HighsSolution& solution, const SimplexScale scale) {
  HighsInt num_col = solution.col_value.size();
  HighsInt num_row = solution.row_value.size();

  for (HighsInt iCol = 0; iCol < num_col; iCol++) {
    solution.col_value[iCol] *= scale.col[iCol];
    solution.col_dual[iCol] /= (scale.col[iCol] / scale.cost);
  }
  for (HighsInt iRow = 0; iRow < num_row; iRow++) {
    solution.row_value[iRow] /= scale.row[iRow];
    solution.row_dual[iRow] *= (scale.row[iRow] * scale.cost);
  }
}

HighsStatus deleteScale(const HighsLogOptions& log_options,
                        vector<double>& scale,
                        const HighsIndexCollection& index_collection) {
  HighsStatus return_status = HighsStatus::kOk;
  if (!assessIndexCollection(log_options, index_collection))
    return interpretCallStatus(HighsStatus::kError, return_status,
                               "assessIndexCollection");
  HighsInt from_k;
  HighsInt to_k;
  if (!limitsForIndexCollection(log_options, index_collection, from_k, to_k))
    return interpretCallStatus(HighsStatus::kError, return_status,
                               "limitsForIndexCollection");
  if (index_collection.is_set_) {
    // For deletion by set it must be increasing
    if (!increasingSetOk(index_collection.set_,
                         index_collection.set_num_entries_, 0,
                         index_collection.dimension_ - 1, true))
      return HighsStatus::kError;
  }
  if (from_k > to_k) return HighsStatus::kOk;

  HighsInt delete_from_col;
  HighsInt delete_to_col;
  HighsInt keep_from_col;
  HighsInt keep_to_col = -1;
  HighsInt current_set_entry = 0;

  HighsInt col_dim = index_collection.dimension_;
  HighsInt new_num_col = 0;
  for (HighsInt k = from_k; k <= to_k; k++) {
    updateIndexCollectionOutInIndex(index_collection, delete_from_col,
                                    delete_to_col, keep_from_col, keep_to_col,
                                    current_set_entry);
    // Account for the initial columns being kept
    if (k == from_k) new_num_col = delete_from_col;
    if (delete_to_col >= col_dim - 1) break;
    assert(delete_to_col < col_dim);
    for (HighsInt col = keep_from_col; col <= keep_to_col; col++) {
      assert((HighsInt)scale.size() > new_num_col);
      scale[new_num_col] = scale[col];
      new_num_col++;
    }
    if (keep_to_col >= col_dim - 1) break;
  }
  return HighsStatus::kOk;
}

void getUnscaledInfeasibilities(const HighsOptions& options, const HighsLp& lp,
                                const SimplexBasis& basis,
                                const HighsSimplexInfo& info,
                                const SimplexScale& scale,
                                HighsSolutionParams& solution_params,
                                const bool scaled_simplex_lp) {
  const double primal_feasibility_tolerance =
      options.primal_feasibility_tolerance;
  const double dual_feasibility_tolerance = options.dual_feasibility_tolerance;

  HighsInt& num_primal_infeasibility = solution_params.num_primal_infeasibility;
  double& max_primal_infeasibility = solution_params.max_primal_infeasibility;
  double& sum_primal_infeasibility = solution_params.sum_primal_infeasibility;
  HighsInt& num_dual_infeasibility = solution_params.num_dual_infeasibility;
  double& max_dual_infeasibility = solution_params.max_dual_infeasibility;
  double& sum_dual_infeasibility = solution_params.sum_dual_infeasibility;

  // Zero the counts of unscaled primal and dual infeasibilities
  num_primal_infeasibility = 0;
  max_primal_infeasibility = 0;
  sum_primal_infeasibility = 0;
  num_dual_infeasibility = 0;
  max_dual_infeasibility = 0;
  sum_dual_infeasibility = 0;

  double scale_mu = 1.0;
  assert(int(scale.col.size()) == lp.num_col_);
  assert(int(scale.row.size()) == lp.num_row_);
  for (HighsInt iVar = 0; iVar < lp.num_col_ + lp.num_row_; iVar++) {
    // Look at the dual infeasibilities of nonbasic variables
    if (basis.nonbasicFlag_[iVar] == kNonbasicFlagFalse) continue;
    // No dual infeasiblity for fixed rows and columns
    if (info.workLower_[iVar] == info.workUpper_[iVar]) continue;
    if (scaled_simplex_lp) {
      bool col = iVar < lp.num_col_;
      HighsInt iCol = 0;
      HighsInt iRow = 0;
      if (col) {
        iCol = iVar;
        assert(int(scale.col.size()) > iCol);
        scale_mu = 1 / (scale.col[iCol] / scale.cost);
      } else {
        iRow = iVar - lp.num_col_;
        assert(int(scale.row.size()) > iRow);
        scale_mu = scale.row[iRow] * scale.cost;
      }
    }
    const double dual = info.workDual_[iVar];
    const double lower = info.workLower_[iVar];
    const double upper = info.workUpper_[iVar];
    const double unscaled_dual = dual * scale_mu;

    double dual_infeasibility;
    if (highs_isInfinity(-lower) && highs_isInfinity(upper)) {
      // Free: any nonzero dual value is infeasible
      dual_infeasibility = fabs(unscaled_dual);
    } else {
      // Not fixed: any dual infeasibility is given by value signed by
      // nonbasicMove. This assumes that nonbasicMove=0 for fixed
      // variables
      dual_infeasibility = -basis.nonbasicMove_[iVar] * unscaled_dual;
    }
    if (dual_infeasibility > 0) {
      if (dual_infeasibility >= dual_feasibility_tolerance)
        num_dual_infeasibility++;
      max_dual_infeasibility = max(dual_infeasibility, max_dual_infeasibility);
      sum_dual_infeasibility += dual_infeasibility;
    }
  }
  // Look at the primal infeasibilities of basic variables
  for (HighsInt ix = 0; ix < lp.num_row_; ix++) {
    HighsInt iVar = basis.basicIndex_[ix];
    if (scaled_simplex_lp) {
      bool col = iVar < lp.num_col_;
      HighsInt iCol = 0;
      HighsInt iRow = 0;
      if (col) {
        iCol = iVar;
        scale_mu = scale.col[iCol];
      } else {
        iRow = iVar - lp.num_col_;
        scale_mu = 1 / scale.row[iRow];
      }
    }
    double unscaled_lower = info.baseLower_[ix] * scale_mu;
    double unscaled_value = info.baseValue_[ix] * scale_mu;
    double unscaled_upper = info.baseUpper_[ix] * scale_mu;
    // @primal_infeasibility calculation
    double primal_infeasibility = 0;
    if (unscaled_value < unscaled_lower - primal_feasibility_tolerance) {
      primal_infeasibility = unscaled_lower - unscaled_value;
    } else if (unscaled_value > unscaled_upper + primal_feasibility_tolerance) {
      primal_infeasibility = unscaled_value - unscaled_upper;
    }
    if (primal_infeasibility > 0) {
      num_primal_infeasibility++;
      max_primal_infeasibility =
          max(primal_infeasibility, max_primal_infeasibility);
      sum_primal_infeasibility += primal_infeasibility;
    }
  }
}

// SCALING

// void initialiseScale(HighsModelObject& highs_model_object) {
// initialiseScale(highs_model_object.lp_, highs_model_object.scale_);}

void initialiseScale(const HighsLp& lp, SimplexScale& scale) {
  scale.is_scaled = false;
  scale.col.assign(lp.num_col_, 1);
  scale.row.assign(lp.num_row_, 1);
  scale.cost = 1;
}

void scaleSimplexLp(HighsOptions& options, HighsLp& lp, SimplexScale& scale) {
  initialiseScale(lp, scale);
  HighsInt numCol = lp.num_col_;
  HighsInt numRow = lp.num_row_;
  // Scaling not well defined for models with no columns
  assert(numCol > 0);
  double* colScale = &scale.col[0];
  double* rowScale = &scale.row[0];
  double* colCost = &lp.col_cost_[0];
  double* colLower = &lp.col_lower_[0];
  double* colUpper = &lp.col_upper_[0];
  double* rowLower = &lp.row_lower_[0];
  double* rowUpper = &lp.row_upper_[0];

  // Save the simplex_scale_strategy so that the option can be
  // modified for the course of this method
  HighsInt simplex_scale_strategy = options.simplex_scale_strategy;
  // Determine the actual strategy to use
  HighsInt use_scale_strategy = simplex_scale_strategy;
  if (use_scale_strategy == kSimplexScaleStrategyChoose) {
    // HiGHS is left to choose: currently use forced equilibration, but maybe do
    // something more intelligent
    use_scale_strategy = kSimplexScaleStrategyForcedEquilibration;
  }
  // Record the strategy to be used
  options.simplex_scale_strategy = use_scale_strategy;

  bool allow_cost_scaling = options.allowed_simplex_cost_scale_factor > 0;
  // Find out range of matrix values and skip matrix scaling if all
  // |values| are in [0.2, 5]
  const double no_scaling_original_matrix_min_value = 0.2;
  const double no_scaling_original_matrix_max_value = 5.0;
  double original_matrix_min_value = kHighsInf;
  double original_matrix_max_value = 0;
  lp.a_matrix_.range(original_matrix_min_value, original_matrix_max_value);
  bool no_scaling =
      (original_matrix_min_value >= no_scaling_original_matrix_min_value) &&
      (original_matrix_max_value <= no_scaling_original_matrix_max_value);
  const bool force_scaling = false;
  if (force_scaling) {
    no_scaling = false;
    printf("!!!! FORCE SCALING !!!!\n");
  }
  bool scaled_matrix = false;
  if (no_scaling) {
    // No matrix scaling, but possible cost scaling
    if (options.highs_debug_level)
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Scaling: Matrix has [min, max] values of [%g, %g] within "
                  "[%g, %g] so no scaling performed\n",
                  original_matrix_min_value, original_matrix_max_value,
                  no_scaling_original_matrix_min_value,
                  no_scaling_original_matrix_max_value);
  } else {
    const bool equilibration_scaling =
        use_scale_strategy == kSimplexScaleStrategyEquilibration ||
        use_scale_strategy == kSimplexScaleStrategyForcedEquilibration;
    if (equilibration_scaling) {
      scaled_matrix = equilibrationScaleSimplexMatrix(options, lp, scale);
    } else {
      scaled_matrix = maxValueScaleSimplexMatrix(options, lp, scale);
    }
    scale.is_scaled = scaled_matrix;
    if (scaled_matrix) {
      // Matrix is scaled, so scale the bounds and costs
      for (HighsInt iCol = 0; iCol < numCol; iCol++) {
        colLower[iCol] /= colScale[iCol];
        colUpper[iCol] /= colScale[iCol];
        colCost[iCol] *= colScale[iCol];
      }
      for (HighsInt iRow = 0; iRow < numRow; iRow++) {
        rowLower[iRow] *= rowScale[iRow];
        rowUpper[iRow] *= rowScale[iRow];
      }
    }
  }
  // Possibly scale the costs
  if (allow_cost_scaling) scaleSimplexCost(options, lp, scale.cost);

  // If matrix is unscaled, then LP is only scaled if there is a cost scaling
  // factor
  if (!scaled_matrix) scale.is_scaled = scale.cost != 1;
  // Record the scaling strategy used and recover the scaling strategy option
  scale.strategy = use_scale_strategy;
  options.simplex_scale_strategy = simplex_scale_strategy;
}

void scaleSimplexCost(const HighsOptions& options, HighsLp& lp,
                      double& cost_scale) {
  // Scale the costs by no less than minAlwCostScale
  double max_allowed_cost_scale =
      pow(2.0, options.allowed_simplex_cost_scale_factor);
  double max_nonzero_cost = 0;
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    if (lp.col_cost_[iCol]) {
      max_nonzero_cost = max(fabs(lp.col_cost_[iCol]), max_nonzero_cost);
    }
  }
  // Scaling the costs up effectively increases the dual tolerance to
  // which the problem is solved - so, if the max cost is small the
  // scaling factor pushes it up by a power of 2 so it's close to 1
  // Scaling the costs down effectively decreases the dual tolerance
  // to which the problem is solved - so this can't be done too much
  cost_scale = 1;
  const double ln2 = log(2.0);
  // Scale if the max cost is positive and outside the range [1/16, 16]
  if ((max_nonzero_cost > 0) &&
      ((max_nonzero_cost < (1.0 / 16)) || (max_nonzero_cost > 16))) {
    cost_scale = max_nonzero_cost;
    cost_scale = pow(2.0, floor(log(cost_scale) / ln2 + 0.5));
    cost_scale = min(cost_scale, max_allowed_cost_scale);
  }
  if (cost_scale == 1) {
    highsLogUser(options.log_options, HighsLogType::kInfo,
                 "LP cost vector not scaled down: max cost is %g\n",
                 max_nonzero_cost);
    return;
  }
  // Scale the costs (and record of max_nonzero_cost) by cost_scale, being at
  // most max_allowed_cost_scale
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    lp.col_cost_[iCol] /= cost_scale;
  }
  max_nonzero_cost /= cost_scale;
  highsLogUser(options.log_options, HighsLogType::kInfo,
               "LP cost vector scaled down by %g: max cost is %g\n", cost_scale,
               max_nonzero_cost);
}

void unscaleSimplexCost(HighsLp& lp, double cost_scale) {
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
    lp.col_cost_[iCol] *= cost_scale;
}

bool equilibrationScaleSimplexMatrix(const HighsOptions& options, HighsLp& lp,
                                     SimplexScale& scale) {
  HighsInt numCol = lp.num_col_;
  HighsInt numRow = lp.num_row_;
  double* colScale = &scale.col[0];
  double* rowScale = &scale.row[0];
  HighsInt* Astart = &lp.a_matrix_.start_[0];
  HighsInt* Aindex = &lp.a_matrix_.index_[0];
  double* Avalue = &lp.a_matrix_.value_[0];
  double* colCost = &lp.col_cost_[0];

  HighsInt simplex_scale_strategy = options.simplex_scale_strategy;

  double original_matrix_min_value = kHighsInf;
  double original_matrix_max_value = 0;
  for (HighsInt k = 0, AnX = Astart[numCol]; k < AnX; k++) {
    double value = fabs(Avalue[k]);
    original_matrix_min_value = min(original_matrix_min_value, value);
    original_matrix_max_value = max(original_matrix_max_value, value);
  }

  // Include cost in scaling if minimum nonzero cost is less than 0.1
  double min_nonzero_cost = kHighsInf;
  for (HighsInt i = 0; i < numCol; i++) {
    if (colCost[i]) min_nonzero_cost = min(fabs(colCost[i]), min_nonzero_cost);
  }
  bool include_cost_in_scaling = false;
  include_cost_in_scaling = min_nonzero_cost < 0.1;

  // Limits on scaling factors
  double max_allow_scale;
  double min_allow_scale;
  // Now that kHighsInf =
  // std::numeric_limits<double>::infinity(), this Qi-trick doesn't
  // work so, in recognition, use the old value of kHighsInf
  const double finite_infinity = 1e200;
  max_allow_scale = pow(2.0, options.allowed_simplex_matrix_scale_factor);
  min_allow_scale = 1 / max_allow_scale;

  double min_allow_col_scale = min_allow_scale;
  double max_allow_col_scale = max_allow_scale;
  double min_allow_row_scale = min_allow_scale;
  double max_allow_row_scale = max_allow_scale;

  // Search up to 6 times
  vector<double> row_min_value(numRow, finite_infinity);
  vector<double> row_max_value(numRow, 1 / finite_infinity);
  for (HighsInt search_count = 0; search_count < 6; search_count++) {
    // Find column scale, prepare row data
    for (HighsInt iCol = 0; iCol < numCol; iCol++) {
      // For column scale (find)
      double col_min_value = finite_infinity;
      double col_max_value = 1 / finite_infinity;
      double abs_col_cost = fabs(colCost[iCol]);
      if (include_cost_in_scaling && abs_col_cost != 0) {
        col_min_value = min(col_min_value, abs_col_cost);
        col_max_value = max(col_max_value, abs_col_cost);
      }
      for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
        double value = fabs(Avalue[k]) * rowScale[Aindex[k]];
        col_min_value = min(col_min_value, value);
        col_max_value = max(col_max_value, value);
      }
      double col_equilibration = 1 / sqrt(col_min_value * col_max_value);
      // Ensure that column scale factor is not excessively large or small
      colScale[iCol] =
          min(max(min_allow_col_scale, col_equilibration), max_allow_col_scale);
      // For row scale (only collect)
      for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
        HighsInt iRow = Aindex[k];
        double value = fabs(Avalue[k]) * colScale[iCol];
        row_min_value[iRow] = min(row_min_value[iRow], value);
        row_max_value[iRow] = max(row_max_value[iRow], value);
      }
    }
    // For row scale (find)
    for (HighsInt iRow = 0; iRow < numRow; iRow++) {
      double row_equilibration =
          1 / sqrt(row_min_value[iRow] * row_max_value[iRow]);
      // Ensure that row scale factor is not excessively large or small
      rowScale[iRow] =
          min(max(min_allow_row_scale, row_equilibration), max_allow_row_scale);
    }
    row_min_value.assign(numRow, finite_infinity);
    row_max_value.assign(numRow, 1 / finite_infinity);
  }
  // Make it numerically better
  // Also determine the max and min row and column scaling factors
  double min_col_scale = finite_infinity;
  double max_col_scale = 1 / finite_infinity;
  double min_row_scale = finite_infinity;
  double max_row_scale = 1 / finite_infinity;
  const double log2 = log(2.0);
  for (HighsInt iCol = 0; iCol < numCol; iCol++) {
    colScale[iCol] = pow(2.0, floor(log(colScale[iCol]) / log2 + 0.5));
    min_col_scale = min(colScale[iCol], min_col_scale);
    max_col_scale = max(colScale[iCol], max_col_scale);
  }
  for (HighsInt iRow = 0; iRow < numRow; iRow++) {
    rowScale[iRow] = pow(2.0, floor(log(rowScale[iRow]) / log2 + 0.5));
    min_row_scale = min(rowScale[iRow], min_row_scale);
    max_row_scale = max(rowScale[iRow], max_row_scale);
  }
  // Apply scaling to matrix and bounds
  double matrix_min_value = finite_infinity;
  double matrix_max_value = 0;
  double min_original_col_equilibration = finite_infinity;
  double sum_original_log_col_equilibration = 0;
  double max_original_col_equilibration = 0;
  double min_original_row_equilibration = finite_infinity;
  double sum_original_log_row_equilibration = 0;
  double max_original_row_equilibration = 0;
  double min_col_equilibration = finite_infinity;
  double sum_log_col_equilibration = 0;
  double max_col_equilibration = 0;
  double min_row_equilibration = finite_infinity;
  double sum_log_row_equilibration = 0;
  double max_row_equilibration = 0;
  vector<double> original_row_min_value(numRow, finite_infinity);
  vector<double> original_row_max_value(numRow, 1 / finite_infinity);
  row_min_value.assign(numRow, finite_infinity);
  row_max_value.assign(numRow, 1 / finite_infinity);
  for (HighsInt iCol = 0; iCol < numCol; iCol++) {
    double original_col_min_value = finite_infinity;
    double original_col_max_value = 1 / finite_infinity;
    double col_min_value = finite_infinity;
    double col_max_value = 1 / finite_infinity;
    for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
      HighsInt iRow = Aindex[k];
      const double original_value = fabs(Avalue[k]);
      original_col_min_value = min(original_value, original_col_min_value);
      original_col_max_value = max(original_value, original_col_max_value);
      original_row_min_value[iRow] =
          min(original_row_min_value[iRow], original_value);
      original_row_max_value[iRow] =
          max(original_row_max_value[iRow], original_value);
      Avalue[k] *= (colScale[iCol] * rowScale[iRow]);
      const double value = fabs(Avalue[k]);
      col_min_value = min(value, col_min_value);
      col_max_value = max(value, col_max_value);
      row_min_value[iRow] = min(row_min_value[iRow], value);
      row_max_value[iRow] = max(row_max_value[iRow], value);
    }
    matrix_min_value = min(matrix_min_value, col_min_value);
    matrix_max_value = max(matrix_max_value, col_max_value);

    const double original_col_equilibration =
        1 / sqrt(original_col_min_value * original_col_max_value);
    min_original_col_equilibration =
        min(original_col_equilibration, min_original_col_equilibration);
    sum_original_log_col_equilibration += log(original_col_equilibration);
    max_original_col_equilibration =
        max(original_col_equilibration, max_original_col_equilibration);
    const double col_equilibration = 1 / sqrt(col_min_value * col_max_value);
    min_col_equilibration = min(col_equilibration, min_col_equilibration);
    sum_log_col_equilibration += log(col_equilibration);
    max_col_equilibration = max(col_equilibration, max_col_equilibration);
  }

  for (HighsInt iRow = 0; iRow < numRow; iRow++) {
    const double original_row_equilibration =
        1 / sqrt(original_row_min_value[iRow] * original_row_max_value[iRow]);
    min_original_row_equilibration =
        min(original_row_equilibration, min_original_row_equilibration);
    sum_original_log_row_equilibration += log(original_row_equilibration);
    max_original_row_equilibration =
        max(original_row_equilibration, max_original_row_equilibration);
    const double row_equilibration =
        1 / sqrt(row_min_value[iRow] * row_max_value[iRow]);
    min_row_equilibration = min(row_equilibration, min_row_equilibration);
    sum_log_row_equilibration += log(row_equilibration);
    max_row_equilibration = max(row_equilibration, max_row_equilibration);
  }
  const double geomean_original_col_equilibration =
      exp(sum_original_log_col_equilibration / numCol);
  const double geomean_original_row_equilibration =
      exp(sum_original_log_row_equilibration / numRow);
  const double geomean_col_equilibration =
      exp(sum_log_col_equilibration / numCol);
  const double geomean_row_equilibration =
      exp(sum_log_row_equilibration / numRow);
  if (options.highs_debug_level) {
    highsLogDev(
        options.log_options, HighsLogType::kInfo,
        "Scaling: Original equilibration: min/mean/max %11.4g/%11.4g/%11.4g "
        "(cols); min/mean/max %11.4g/%11.4g/%11.4g (rows)\n",
        min_original_col_equilibration, geomean_original_col_equilibration,
        max_original_col_equilibration, min_original_row_equilibration,
        geomean_original_row_equilibration, max_original_row_equilibration);
    highsLogDev(
        options.log_options, HighsLogType::kInfo,
        "Scaling: Final    equilibration: min/mean/max %11.4g/%11.4g/%11.4g "
        "(cols); min/mean/max %11.4g/%11.4g/%11.4g (rows)\n",
        min_col_equilibration, geomean_col_equilibration, max_col_equilibration,
        min_row_equilibration, geomean_row_equilibration,
        max_row_equilibration);
  }

  // Compute the mean equilibration improvement
  const double geomean_original_col =
      max(geomean_original_col_equilibration,
          1 / geomean_original_col_equilibration);
  const double geomean_original_row =
      max(geomean_original_row_equilibration,
          1 / geomean_original_row_equilibration);
  const double geomean_col =
      max(geomean_col_equilibration, 1 / geomean_col_equilibration);
  const double geomean_row =
      max(geomean_row_equilibration, 1 / geomean_row_equilibration);
  const double mean_equilibration_improvement =
      (geomean_original_col * geomean_original_row) /
      (geomean_col * geomean_row);
  // Compute the extreme equilibration improvement
  const double original_col_ratio =
      max_original_col_equilibration / min_original_col_equilibration;
  const double original_row_ratio =
      max_original_row_equilibration / min_original_row_equilibration;
  const double col_ratio = max_col_equilibration / min_col_equilibration;
  const double row_ratio = max_row_equilibration / min_row_equilibration;
  const double extreme_equilibration_improvement =
      (original_col_ratio + original_row_ratio) / (col_ratio + row_ratio);
  // Compute the max/min matrix value improvement
  const double matrix_value_ratio = matrix_max_value / matrix_min_value;
  const double original_matrix_value_ratio =
      original_matrix_max_value / original_matrix_min_value;
  const double matrix_value_ratio_improvement =
      original_matrix_value_ratio / matrix_value_ratio;
  if (options.highs_debug_level) {
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling: Extreme equilibration improvement = ( %11.4g + "
                "%11.4g) / ( %11.4g + %11.4g) = %11.4g / %11.4g = %11.4g\n",
                original_col_ratio, original_row_ratio, col_ratio, row_ratio,
                (original_col_ratio + original_row_ratio),
                (col_ratio + row_ratio), extreme_equilibration_improvement);
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling:    Mean equilibration improvement = ( %11.4g * "
                "%11.4g) / ( %11.4g * %11.4g) = %11.4g / %11.4g = %11.4g\n",
                geomean_original_col, geomean_original_row, geomean_col,
                geomean_row, (geomean_original_col * geomean_original_row),
                (geomean_col * geomean_row), mean_equilibration_improvement);
    highsLogDev(
        options.log_options, HighsLogType::kInfo,
        "Scaling: Yields [min, max, ratio] matrix values of [%0.4g, %0.4g, "
        "%0.4g]; Originally [%0.4g, %0.4g, %0.4g]: Improvement of %0.4g\n",
        matrix_min_value, matrix_max_value, matrix_value_ratio,
        original_matrix_min_value, original_matrix_max_value,
        original_matrix_value_ratio, matrix_value_ratio_improvement);
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling: Improves    mean equilibration by a factor %0.4g\n",
                mean_equilibration_improvement);
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling: Improves extreme equilibration by a factor %0.4g\n",
                extreme_equilibration_improvement);
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling: Improves max/min matrix values by a factor %0.4g\n",
                matrix_value_ratio_improvement);
  }
  const bool possibly_abandon_scaling =
      simplex_scale_strategy != kSimplexScaleStrategyForcedEquilibration;
  const double improvement_factor = extreme_equilibration_improvement *
                                    mean_equilibration_improvement *
                                    matrix_value_ratio_improvement;

  const double improvement_factor_required = 1.0;
  const bool poor_improvement =
      improvement_factor < improvement_factor_required;

  // Possibly abandon scaling if it's not improved equlibration significantly
  if (possibly_abandon_scaling && poor_improvement) {
    // Unscale the matrix
    for (HighsInt iCol = 0; iCol < numCol; iCol++) {
      for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
        HighsInt iRow = Aindex[k];
        Avalue[k] /= (colScale[iCol] * rowScale[iRow]);
      }
    }
    if (options.highs_debug_level)
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Scaling: Improvement factor %0.4g < %0.4g required, so no "
                  "scaling applied\n",
                  improvement_factor, improvement_factor_required);
    initialiseScale(lp, scale);
    return false;
  } else {
    if (options.highs_debug_level) {
      highsLogDev(options.log_options, HighsLogType::kInfo,
                  "Scaling: Improvement factor is %0.4g >= %0.4g so scale LP\n",
                  improvement_factor, improvement_factor_required);
      if (extreme_equilibration_improvement < 1.0) {
        highsLogDev(
            options.log_options, HighsLogType::kWarning,
            "Scaling: Applying scaling with extreme improvement of %0.4g\n",
            extreme_equilibration_improvement);
      }
      if (mean_equilibration_improvement < 1.0) {
        highsLogDev(
            options.log_options, HighsLogType::kWarning,
            "Scaling: Applying scaling with mean improvement of %0.4g\n",
            mean_equilibration_improvement);
      }
      if (matrix_value_ratio_improvement < 1.0) {
        highsLogDev(options.log_options, HighsLogType::kWarning,
                    "Scaling: Applying scaling with matrix value ratio "
                    "improvement of %0.4g\n",
                    matrix_value_ratio_improvement);
      }
      if (improvement_factor < 10 * improvement_factor_required) {
        highsLogDev(options.log_options, HighsLogType::kWarning,
                    "Scaling: Applying scaling with improvement factor %0.4g "
                    "< 10*(%0.4g) improvement\n",
                    improvement_factor, improvement_factor_required);
      }
    }
  }
  return true;
}

bool maxValueScaleSimplexMatrix(const HighsOptions& options, HighsLp& lp,
                                SimplexScale& scale) {
  HighsInt numCol = lp.num_col_;
  HighsInt numRow = lp.num_row_;
  vector<double>& colScale = scale.col;
  vector<double>& rowScale = scale.row;
  vector<HighsInt>& Astart = lp.a_matrix_.start_;
  vector<HighsInt>& Aindex = lp.a_matrix_.index_;
  vector<double>& Avalue = lp.a_matrix_.value_;

  assert(options.simplex_scale_strategy == kSimplexScaleStrategyMaxValue015 ||
         options.simplex_scale_strategy == kSimplexScaleStrategyMaxValue0157);
  const double log2 = log(2.0);
  const double max_allow_scale =
      pow(2.0, options.allowed_simplex_matrix_scale_factor);
  const double min_allow_scale = 1 / max_allow_scale;

  const double min_allow_col_scale = min_allow_scale;
  const double max_allow_col_scale = max_allow_scale;
  const double min_allow_row_scale = min_allow_scale;
  const double max_allow_row_scale = max_allow_scale;

  double min_row_scale = kHighsInf;
  double max_row_scale = 0;
  double original_matrix_min_value = kHighsInf;
  double original_matrix_max_value = 0;
  // Determine the row scaling. Also determine the max/min row scaling
  // factors, and max/min original matrix values
  vector<double> row_max_value(numRow, 0);
  for (HighsInt iCol = 0; iCol < numCol; iCol++) {
    for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
      const HighsInt iRow = Aindex[k];
      const double value = fabs(Avalue[k]);
      row_max_value[iRow] = max(row_max_value[iRow], value);
      original_matrix_min_value = min(original_matrix_min_value, value);
      original_matrix_max_value = max(original_matrix_max_value, value);
    }
  }
  for (HighsInt iRow = 0; iRow < numRow; iRow++) {
    if (row_max_value[iRow]) {
      double row_scale_value = 1 / row_max_value[iRow];
      // Convert the row scale factor to the nearest power of two, and
      // ensure that it is not excessively large or small
      row_scale_value = pow(2.0, floor(log(row_scale_value) / log2 + 0.5));
      row_scale_value =
          min(max(min_allow_row_scale, row_scale_value), max_allow_row_scale);
      min_row_scale = min(row_scale_value, min_row_scale);
      max_row_scale = max(row_scale_value, max_row_scale);
      rowScale[iRow] = row_scale_value;
    }
  }
  // Determine the column scaling, whilst applying the row scaling
  // Also determine the max/min column scaling factors, and max/min
  // matrix values
  double min_col_scale = kHighsInf;
  double max_col_scale = 0;
  double matrix_min_value = kHighsInf;
  double matrix_max_value = 0;
  for (HighsInt iCol = 0; iCol < numCol; iCol++) {
    double col_max_value = 0;
    for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
      const HighsInt iRow = Aindex[k];
      Avalue[k] *= rowScale[iRow];
      const double value = fabs(Avalue[k]);
      col_max_value = max(col_max_value, value);
    }
    if (col_max_value) {
      double col_scale_value = 1 / col_max_value;
      // Convert the col scale factor to the nearest power of two, and
      // ensure that it is not excessively large or small
      col_scale_value = pow(2.0, floor(log(col_scale_value) / log2 + 0.5));
      col_scale_value =
          min(max(min_allow_col_scale, col_scale_value), max_allow_col_scale);
      min_col_scale = min(col_scale_value, min_col_scale);
      max_col_scale = max(col_scale_value, max_col_scale);
      colScale[iCol] = col_scale_value;
      for (HighsInt k = Astart[iCol]; k < Astart[iCol + 1]; k++) {
        Avalue[k] *= colScale[iCol];
        const double value = fabs(Avalue[k]);
        matrix_min_value = min(matrix_min_value, value);
        matrix_max_value = max(matrix_max_value, value);
      }
    }
  }
  const double matrix_value_ratio = matrix_max_value / matrix_min_value;
  const double original_matrix_value_ratio =
      original_matrix_max_value / original_matrix_min_value;
  const double matrix_value_ratio_improvement =
      original_matrix_value_ratio / matrix_value_ratio;
  if (options.highs_debug_level) {
    highsLogDev(options.log_options, HighsLogType::kInfo,
                "Scaling: Factors are in [%0.4g, %0.4g] for columns and in "
                "[%0.4g, %0.4g] for rows\n",
                min_col_scale, max_col_scale, min_row_scale, max_row_scale);
    highsLogDev(
        options.log_options, HighsLogType::kInfo,
        "Scaling: Yields [min, max, ratio] matrix values of [%0.4g, %0.4g, "
        "%0.4g]; Originally [%0.4g, %0.4g, %0.4g]: Improvement of %0.4g\n",
        matrix_min_value, matrix_max_value, matrix_value_ratio,
        original_matrix_min_value, original_matrix_max_value,
        original_matrix_value_ratio, matrix_value_ratio_improvement);
  }
  return true;
}

bool isBasisRightSize(const HighsLp& lp, const SimplexBasis& basis) {
  bool right_size = true;
  right_size =
      (HighsInt)basis.nonbasicFlag_.size() == lp.num_col_ + lp.num_row_ &&
      right_size;
  right_size =
      (HighsInt)basis.nonbasicMove_.size() == lp.num_col_ + lp.num_row_ &&
      right_size;
  right_size = (HighsInt)basis.basicIndex_.size() == lp.num_row_ && right_size;
  return right_size;
}

void scaleSimplexLp(HighsLp& lp, const SimplexScale& scale, const bool force) {
  // If the LP isscaled, then return
  if (scale.is_scaled && !force) return;
  // Scale the bounds and costs and matrix
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    lp.col_lower_[iCol] /= scale.col[iCol];
    lp.col_upper_[iCol] /= scale.col[iCol];
    lp.col_cost_[iCol] *= scale.col[iCol];
  }
  for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++) {
    lp.row_lower_[iRow] *= scale.row[iRow];
    lp.row_upper_[iRow] *= scale.row[iRow];
  }
  lp.a_matrix_.applyScale(scale);
  //  scale.is_scaled = true;
}

void unscaleSimplexLp(HighsLp& lp, const SimplexScale& scale,
                      const bool force) {
  // If the LP isn't scaled, then return
  if (!scale.is_scaled && !force) return;
  // Unscale the bounds and costs and matrix
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    lp.col_lower_[iCol] *= scale.col[iCol];
    lp.col_upper_[iCol] *= scale.col[iCol];
    lp.col_cost_[iCol] /= scale.col[iCol];
  }
  for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++) {
    lp.row_lower_[iRow] /= scale.row[iRow];
    lp.row_upper_[iRow] /= scale.row[iRow];
  }
  lp.a_matrix_.unapplyScale(scale);
  //  scale.is_scaled = false;
}
