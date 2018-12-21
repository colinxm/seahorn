#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"

#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Support/CFG.hh"
#include "seahorn/VCGen.hh"
#include "ufo/Stats.hh"
#include "ufo/Smt/EZ3.hh"

#include "avy/AvyDebug.h"

static llvm::cl::opt<bool> SplitCriticalEdgesOnly(
    "horn-split-only-critical",
    llvm::cl::desc("Introduce edge variables only for critical edges"),
    llvm::cl::init(true), llvm::cl::Hidden);

static llvm::cl::opt<bool> EnforceAtMostOnePredecessor(
    "horn-at-most-one-predecessor",
    llvm::cl::desc("Encode a block has at most one predecessor"),
    llvm::cl::init(false), llvm::cl::Hidden);

static llvm::cl::opt<bool> LargeStepReduce(
    "horn-large-reduce",
    llvm::cl::desc("Reduce constraints during large-step encoding"),
    llvm::cl::init(false), llvm::cl::Hidden);

namespace seahorn {
using namespace ufo;

void VCGen::initSmt(std::unique_ptr<ufo::EZ3> &zctx,
                    std::unique_ptr<ufo::ZSolver<ufo::EZ3> > &smt) {
  if (!LargeStepReduce) return;
  errs() << "\nE";
  // XXX Consider using global EZ3
  zctx.reset(new EZ3(m_sem.efac()));
  smt.reset(new ZSolver<EZ3>(*zctx));
  ZParams<EZ3> params(*zctx);
  params.set(":smt.array.weak", true);
  params.set(":smt.arith.ignore_int", true);
  smt->set(params);
}

void VCGen::checkSideAtBb(unsigned &head,
                          ExprVector &side,
                          Expr bbV, ZSolver<EZ3> &smt,
                          const CpEdge &edge,
                          const BasicBlock &bb) {
  if (!LargeStepReduce) return;
  bind::IsConst isConst;

  for (unsigned sz = side.size(); head < sz; ++head) {
    ufo::ScopedStats __st__("VCGen.smt");
    Expr e = side[head];
    if (!bind::isFapp(e) || isConst(e))
      smt.assertExpr(e);
  }

  errs() << ".";
  errs().flush();

  TimeIt<llvm::raw_ostream &> _t_("smt-solving", errs(), 0.1);

  ufo::ScopedStats __st__("VCGen.smt");
  Expr a[1] = {bbV};

  LOG("pedge", std::error_code EC;
      raw_fd_ostream file("/tmp/p-edge.smt2", EC, sys::fs::F_Text);
      if (!EC) {
        file << "(set-info :original \"" << edge.source().bb().getName()
             << " --> " << bb.getName() << " --> "
             << edge.target().bb().getName() << "\")\n";
        smt.toSmtLibAssuming(file, a);
        file.close();
      });

  try {
    auto res = smt.solveAssuming(a);
    if (!res) {
      errs() << "F";
      errs().flush();
      Stats::count("VCGen.smt.unsat");
      smt.assertExpr(boolop::lneg(bbV));
      side.push_back(boolop::lneg(bbV));
    }
  } catch (z3::exception &e) {
    errs() << e.msg() << "\n";
    // std::exit (1);
  }
}

void VCGen::checkSideAtEnd(unsigned &head, ExprVector &side,
                           ufo::ZSolver<ufo::EZ3> &smt) {
  if (!LargeStepReduce) return;
  bind::IsConst isConst;
    for (unsigned sz = side.size(); head < sz; ++head) {
      ufo::ScopedStats __st__("VCGen.smt");
      Expr e = side[head];
      if (!bind::isFapp(e) || isConst(e))
        smt.assertExpr(e);
    }

    try {
      ufo::ScopedStats __st__("VCGen.smt.last");
      auto res = smt.solve();
      if (!res) {
        Stats::count("VCGen.smt.last.unsat");
        side.push_back(mk<FALSE>(m_sem.efac()));
      }
    } catch (z3::exception &e) {
      errs() << e.msg() << "\n";
    }
}


void VCGen::genVcForCpEdge(SymStore &s, const CpEdge &edge, ExprVector &side) {
  const CutPoint &target = edge.target();

  std::unique_ptr<EZ3> zctx;
  std::unique_ptr<ZSolver<EZ3>> smt;
  initSmt(zctx, smt);

  // remember what was added since last call to smt
  unsigned head = side.size();

  bool isEntry = true;
  for (const BasicBlock &bb : edge) {
    Expr bbV;
    if (isEntry) {
      // -- initialize bb-exec register
      bbV = s.havoc(m_sem.symb(bb));
      // -- compute side-conditions for the entry block of the edge
      m_sem.exec(s, bb, side, trueE);
    } else {
      // -- generate side-conditions for bb
      genVcForBasicBlockOnEdge(s, edge, bb, side);
      // -- check current value of bb-exec register
      bbV = s.read(m_sem.symb(bb));
    }
    isEntry = false;

    // -- check that the current side condition is consistent
    checkSideAtBb(head, side, bbV, *smt, edge, bb);
  }

  // -- generate side condition for the last basic block on the edge
  // -- this executes only PHINode instructions in target.bb()
  genVcForBasicBlockOnEdge(s, edge, target.bb(), side, true);

  // -- check consistency of side-conditions at the end
  checkSideAtEnd(head, side, *smt);
}

namespace sem_detail {
struct FwdReachPred : public std::unary_function<const BasicBlock &, bool> {
  const CutPointGraph &m_cpg;
  const CutPoint &m_cp;

  FwdReachPred(const CutPointGraph &cpg, const CutPoint &cp)
      : m_cpg(cpg), m_cp(cp) {}

  bool operator()(const BasicBlock &bb) const {
    return m_cpg.isFwdReach(m_cp, bb);
  }
  bool operator()(const BasicBlock *bb) const { return this->operator()(*bb); }
};
} // namespace sem_detail


/// \brief Naive quadratic implementation of at-most-one
static Expr mkAtMostOne(ExprVector &vec) {
  assert(!vec.empty());
  ExprVector res;
  for (unsigned i = 0, e = vec.size(); i < e; ++i) {
    ExprVector exactly_vi; // exactly  vec[i]
    exactly_vi.push_back(vec[i]);
    for (unsigned j = 0; j < e; ++j) {
      if (vec[i] == vec[j]) continue;
      exactly_vi.push_back(mk<NEG>(vec[j]));
    }
    // at this point: exactly_vi is vi && !vj for all j
    res.push_back(op::boolop::land(exactly_vi));
  }
  return mknary<OR>(res);
}

void VCGen::genVcForBasicBlockOnEdge(SymStore &s, const CpEdge &edge,
                                     const BasicBlock &bb, ExprVector &side,
                                     bool last) {
  ExprVector edges;

  if (last)
    assert(&bb == &(edge.target().bb()));

  // -- predicate for reachable from source
  sem_detail::FwdReachPred reachable(edge.parent(), edge.source());

  // compute predecessors, relative to the source cut-point
  llvm::SmallVector<const BasicBlock *, 16> preds;
  for (const BasicBlock *p : seahorn::preds(bb))
    if (reachable(p)) preds.push_back(p);

  // -- compute source of all the edges
  for (const BasicBlock *pred : preds)
    edges.push_back(s.read(m_sem.symb(*pred)));

  assert(preds.size() == edges.size());
  // -- update constant representing current bb
  Expr bbV = s.havoc(m_sem.symb(bb));

  // -- update destination of all the edges

  if (SplitCriticalEdgesOnly) {
    // -- create edge variables only for critical edges.
    // -- for non critical edges, use (SRC && DST) instead

    for (unsigned i = 0, e = preds.size(); i < e; ++i) {
      // -- an edge is non-critical if dst has one predecessor, or src
      // -- has one successor
      if (e == 1 || preds[i]->getTerminator()->getNumSuccessors() == 1)
        // -- single successor is non-critical
        edges[i] = mk<AND>(bbV, edges[i]);
      else // -- critical edge, add edge variable
      {
        Expr edgV = bind::boolConst(mk<TUPLE>(edges[i], bbV));
        side.push_back(mk<IMPL>(edgV, edges[i]));
        edges[i] = mk<AND>(edges[i], edgV);
      }
    }
  } else {
    // -- b_i & e_{i,j}
    for (Expr &e : edges)
      e = mk<AND>(e, bind::boolConst(mk<TUPLE>(e, bbV)));
  }

  if (EnforceAtMostOnePredecessor && edges.size() > 1) {
    // this enforces at-most-one predecessor.
    // if !bbV (i.e., bb is not reachable) then bb won't have
    // predecessors.
    side.push_back(mk<IMPL>(bbV, mkAtMostOne(edges)));
  }

  // -- encode control flow
  // -- b_j -> (b1 & e_{1,j} | b2 & e_{2,j} | ...)
  side.push_back(
      mk<IMPL>(bbV, mknary<OR>(mk<FALSE>(m_sem.efac()), edges)));

  // unique node with no successors is asserted to always be reachable
  if (last)
    side.push_back(bbV);

  /// -- generate constraints from the phi-nodes (keep them separate for now)
  std::vector<ExprVector> phiConstraints(preds.size());

  unsigned idx = 0;
  for (const BasicBlock *pred : preds) {
    // clone s
    SymStore es(s);

    // edge_ij -> phi_ij,
    // -- branch condition
    m_sem.execBr(es, *pred, bb, side, edges[idx]);
    // -- definition of phi nodes
    m_sem.execPhi(es, bb, *pred, side, edges[idx]);
    // -- collect all uses since `es` is local
    s.uses(es.uses());

    for (const Instruction &inst : bb) {
      if (!isa<PHINode>(&inst))
        break;
      if (!m_sem.isTracked(inst))
        continue;

      // -- record the value of PHINode after taking `pred --> bb` edge
      phiConstraints[idx].push_back(es.read(m_sem.symb(inst)));
    }

    idx++;
  }

  // create new values for PHINode registers
  ExprVector newPhi;
  for (const Instruction &inst : bb) {
    if (!isa<PHINode>(inst))
      break;
    if (!m_sem.isTracked(inst))
      continue;
    newPhi.push_back(s.havoc(m_sem.symb(inst)));
  }

  // connect new PHINode register values with constructed PHINode values
  for (unsigned j = 0; j < edges.size(); ++j)
    for (unsigned i = 0; i < newPhi.size(); ++i)
      side.push_back(
          boolop::limp(edges[j], mk<EQ>(newPhi[i], phiConstraints[j][i])));

  // actions of the block. The side-conditions are not guarded by
  // the basic-block variable because it does not matter.
  if (!last)
    m_sem.exec(s, bb, side, bbV);
  else if (const TerminatorInst *term = bb.getTerminator())
    if (isa<UnreachableInst>(term))
      m_sem.exec(s, bb, side, trueE);
}

// 1. execute all basic blocks using small-step semantics in topological order

// 2. when a block executes, it updates a special variable for the block

// 3. allocate a Boolean variable for each edge: these are unique
// if named using bb variables

// 4. side condition: edge -> branchCond & execPhi

// actions: optionally conditional on the block
// bb_i -> e_i1 | e_i2 | e_i3
// e_i1 -> bb_1
// e_i1 -> brcond (i, 1)
// e_i1 -> phi_1(i)
} // namespace seahorn
