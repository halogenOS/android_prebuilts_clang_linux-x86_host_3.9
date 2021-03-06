//===------ polly/ScheduleOptimizer.h - The Schedule Optimizer *- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef POLLY_SCHEDULE_OPTIMIZER_H
#define POLLY_SCHEDULE_OPTIMIZER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "isl/ctx.h"

struct isl_schedule;
struct isl_schedule_node;
struct isl_union_map;

namespace polly {
extern bool DisablePollyTiling;
class Scop;
} // namespace polly

class ScheduleTreeOptimizer {
public:
  /// @brief Apply schedule tree transformations.
  ///
  /// This function takes an (possibly already optimized) schedule tree and
  /// applies a set of additional optimizations on the schedule tree. The
  /// transformations applied include:
  ///
  ///   - Tiling
  ///   - Prevectorization
  ///
  /// @param Schedule The schedule object the transformations will be applied
  ///                 to.
  /// @param TTI      Target Transform Info.
  /// @returns        The transformed schedule.
  static __isl_give isl_schedule *
  optimizeSchedule(__isl_take isl_schedule *Schedule,
                   const llvm::TargetTransformInfo *TTI = nullptr);

  /// @brief Apply schedule tree transformations.
  ///
  /// This function takes a node in an (possibly already optimized) schedule
  /// tree and applies a set of additional optimizations on this schedule tree
  /// node and its descendents. The transformations applied include:
  ///
  ///   - Tiling
  ///   - Prevectorization
  ///
  /// @param Node The schedule object post-transformations will be applied to.
  /// @param TTI  Target Transform Info.
  /// @returns    The transformed schedule.
  static __isl_give isl_schedule_node *
  optimizeScheduleNode(__isl_take isl_schedule_node *Node,
                       const llvm::TargetTransformInfo *TTI = nullptr);

  /// @brief Decide if the @p NewSchedule is profitable for @p S.
  ///
  /// @param S           The SCoP we optimize.
  /// @param NewSchedule The new schedule we computed.
  ///
  /// @return True, if we believe @p NewSchedule is an improvement for @p S.
  static bool isProfitableSchedule(polly::Scop &S,
                                   __isl_keep isl_union_map *NewSchedule);

  /// @brief Isolate a set of partial tile prefixes.
  ///
  /// This set should ensure that it contains only partial tile prefixes that
  /// have exactly VectorWidth iterations.
  ///
  /// @param Node A schedule node band, which is a parent of a band node,
  ///             that contains a vector loop.
  /// @return Modified isl_schedule_node.
  static __isl_give isl_schedule_node *
  isolateFullPartialTiles(__isl_take isl_schedule_node *Node, int VectorWidth);

private:
  /// @brief Tile a schedule node.
  ///
  /// @param Node            The node to tile.
  /// @param Identifier      An name that identifies this kind of tiling and
  ///                        that is used to mark the tiled loops in the
  ///                        generated AST.
  /// @param TileSizes       A vector of tile sizes that should be used for
  ///                        tiling.
  /// @param DefaultTileSize A default tile size that is used for dimensions
  ///                        that are not covered by the TileSizes vector.
  static __isl_give isl_schedule_node *
  tileNode(__isl_take isl_schedule_node *Node, const char *Identifier,
           llvm::ArrayRef<int> TileSizes, int DefaultTileSize);

  /// @brief Tile a schedule node and unroll point loops.
  ///
  /// @param Node            The node to register tile.
  /// @param TileSizes       A vector of tile sizes that should be used for
  ///                        tiling.
  /// @param DefaultTileSize A default tile size that is used for dimensions
  static __isl_give isl_schedule_node *
  applyRegisterTiling(__isl_take isl_schedule_node *Node,
                      llvm::ArrayRef<int> TileSizes, int DefaultTileSize);

  /// @brief Apply the BLIS matmul optimization pattern
  ///
  /// Apply the BLIS matmul optimization pattern. BLIS implements gemm
  /// as three nested loops around a macro-kernel, plus two packing routines.
  /// The macro-kernel is implemented in terms of two additional loops around
  /// a micro-kernel. The micro-kernel is a loop around a rank-1
  /// (i.e., outer product) update.
  ///
  /// For a detailed description please see:
  /// Analytical Modeling is Enough for High Performance BLIS
  /// Tze Meng Low, Francisco D Igual, Tyler M Smith, Enrique S Quintana-Orti
  /// Technical Report, 2014
  /// http://www.cs.utexas.edu/users/flame/pubs/TOMS-BLIS-Analytical.pdf
  ///
  /// We create the BLIS micro-kernel by applying a combination of tiling
  /// and unrolling. In subsequent changes we will add the extraction
  /// of the BLIS macro-kernel and implement the packing transformation.
  ///
  /// It is assumed that the Node is successfully checked
  /// by ScheduleTreeOptimizer::isMatrMultPattern. Consequently
  /// in case of matmul kernels the application of optimizeMatMulPattern
  /// can lead to close-to-peak performance. Maybe it can be generalized
  /// to effectively optimize the whole class of successfully checked
  /// statements.
  ///
  /// @param Node the node that contains a band to be optimized.
  /// @return Modified isl_schedule_node.
  static __isl_give isl_schedule_node *
  optimizeMatMulPattern(__isl_take isl_schedule_node *Node,
                        const llvm::TargetTransformInfo *TTI);

  /// @brief Check if this node is a band node we want to tile.
  ///
  /// We look for innermost band nodes where individual dimensions are marked as
  /// permutable.
  ///
  /// @param Node The node to check.
  static bool isTileableBandNode(__isl_keep isl_schedule_node *Node);

  /// @brief Pre-vectorizes one scheduling dimension of a schedule band.
  ///
  /// prevectSchedBand splits out the dimension DimToVectorize, tiles it and
  /// sinks the resulting point loop.
  ///
  /// Example (DimToVectorize=0, VectorWidth=4):
  ///
  /// | Before transformation:
  /// |
  /// | A[i,j] -> [i,j]
  /// |
  /// | for (i = 0; i < 128; i++)
  /// |    for (j = 0; j < 128; j++)
  /// |      A(i,j);
  ///
  /// | After transformation:
  /// |
  /// | for (it = 0; it < 32; it+=1)
  /// |    for (j = 0; j < 128; j++)
  /// |      for (ip = 0; ip <= 3; ip++)
  /// |        A(4 * it + ip,j);
  ///
  /// The goal of this transformation is to create a trivially vectorizable
  /// loop.  This means a parallel loop at the innermost level that has a
  /// constant number of iterations corresponding to the target vector width.
  ///
  /// This transformation creates a loop at the innermost level. The loop has
  /// a constant number of iterations, if the number of loop iterations at
  /// DimToVectorize can be divided by VectorWidth. The default VectorWidth is
  /// currently constant and not yet target specific. This function does not
  /// reason about parallelism.
  static __isl_give isl_schedule_node *
  prevectSchedBand(__isl_take isl_schedule_node *Node, unsigned DimToVectorize,
                   int VectorWidth);

  /// @brief Apply additional optimizations on the bands in the schedule tree.
  ///
  /// We are looking for an innermost band node and apply the following
  /// transformations:
  ///
  ///  - Tile the band
  ///      - if the band is tileable
  ///      - if the band has more than one loop dimension
  ///
  ///  - Prevectorize the schedule of the band (or the point loop in case of
  ///    tiling).
  ///      - if vectorization is enabled
  ///
  /// @param Node The schedule node to (possibly) optimize.
  /// @param User A pointer to forward some use information
  ///        (currently unused).
  static isl_schedule_node *optimizeBand(isl_schedule_node *Node, void *User);

  /// @brief Apply additional optimizations on the bands in the schedule tree.
  ///
  /// We apply the following
  /// transformations:
  ///
  ///  - Tile the band
  ///  - Prevectorize the schedule of the band (or the point loop in case of
  ///    tiling).
  ///      - if vectorization is enabled
  ///
  /// @param Node The schedule node to (possibly) optimize.
  /// @param User A pointer to forward some use information
  ///        (currently unused).
  static isl_schedule_node *standardBandOpts(__isl_take isl_schedule_node *Node,
                                             void *User);

  /// @brief Check if this node contains a partial schedule that could
  ///        probably be optimized with analytical modeling.
  ///
  /// isMatrMultPattern tries to determine whether the following conditions
  /// are true:
  /// 1. the partial schedule contains only one statement.
  /// 2. there are exactly three input dimensions.
  /// 3. all memory accesses of the statement will have stride 0 or 1, if we
  ///    interchange loops (switch the variable used in the inner loop to
  ///    the outer loop).
  /// 4. all memory accesses of the statement except from the last one, are
  ///    read memory access and the last one is write memory access.
  /// 5. all subscripts of the last memory access of the statement don't
  ///    contain the variable used in the inner loop.
  /// If this is the case, we could try to use an approach that is similar to
  /// the one used to get close-to-peak performance of matrix multiplications.
  ///
  /// @param Node The node to check.
  static bool isMatrMultPattern(__isl_keep isl_schedule_node *Node);
};

#endif
