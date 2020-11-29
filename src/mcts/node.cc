/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "mcts/node.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "neural/encoder.h"
#include "neural/network.h"
#include "utils/exception.h"
#include "utils/fastmath.h"
#include "utils/hashcat.h"

namespace lczero {

/////////////////////////////////////////////////////////////////////////
// Node garbage collector
/////////////////////////////////////////////////////////////////////////

namespace {
// Periodicity of garbage collection, milliseconds.
const int kGCIntervalMs = 100;

// Every kGCIntervalMs milliseconds release nodes in a separate GC thread.
class NodeGarbageCollector {
 public:
  NodeGarbageCollector() : gc_thread_([this]() { Worker(); }) {}

  // Takes ownership of a subtree, to dispose it in a separate thread when
  // it has time.
  void AddToGcQueue(std::unique_ptr<Node> node, size_t solid_size = 0) {
    if (!node) return;
    Mutex::Lock lock(gc_mutex_);
    subtrees_to_gc_.emplace_back(std::move(node));
    subtrees_to_gc_solid_size_.push_back(solid_size);
  }

  ~NodeGarbageCollector() {
    // Flips stop flag and waits for a worker thread to stop.
    stop_.store(true);
    gc_thread_.join();
  }

 private:
  void GarbageCollect() {
    while (!stop_.load()) {
      // Node will be released in destructor when mutex is not locked.
      std::unique_ptr<Node> node_to_gc;
      size_t solid_size = 0;
      {
        // Lock the mutex and move last subtree from subtrees_to_gc_ into
        // node_to_gc.
        Mutex::Lock lock(gc_mutex_);
        if (subtrees_to_gc_.empty()) return;
        node_to_gc = std::move(subtrees_to_gc_.back());
        subtrees_to_gc_.pop_back();
        solid_size = subtrees_to_gc_solid_size_.back();
        subtrees_to_gc_solid_size_.pop_back();
      }
      // Solid is a hack...
      if (solid_size != 0) {
        for (size_t i = 0; i < solid_size; i++) {
          node_to_gc.get()[i].~Node();
        }
        std::allocator<Node> alloc;
        alloc.deallocate(node_to_gc.release(), solid_size);
      }
    }
  }

  void Worker() {
    while (!stop_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kGCIntervalMs));
      GarbageCollect();
    };
  }

  mutable Mutex gc_mutex_;
  std::vector<std::unique_ptr<Node>> subtrees_to_gc_ GUARDED_BY(gc_mutex_);
  std::vector<size_t> subtrees_to_gc_solid_size_ GUARDED_BY(gc_mutex_);

  // When true, Worker() should stop and exit.
  std::atomic<bool> stop_{false};
  std::thread gc_thread_;
};  // namespace

NodeGarbageCollector gNodeGc;
}  // namespace

/////////////////////////////////////////////////////////////////////////
// Edge
/////////////////////////////////////////////////////////////////////////

Move Edge::GetMove(bool as_opponent) const {
  if (!as_opponent) return move_;
  Move m = move_;
  m.Mirror();
  return m;
}

// Policy priors (P) are stored in a compressed 16-bit format.
//
// Source values are 32-bit floats:
// * bit 31 is sign (zero means positive)
// * bit 30 is sign of exponent (zero means nonpositive)
// * bits 29..23 are value bits of exponent
// * bits 22..0 are significand bits (plus a "virtual" always-on bit: s ∈ [1,2))
// The number is then sign * 2^exponent * significand, usually.
// See https://www.h-schmidt.net/FloatConverter/IEEE754.html for details.
//
// In compressed 16-bit value we store bits 27..12:
// * bit 31 is always off as values are always >= 0
// * bit 30 is always off as values are always < 2
// * bits 29..28 are only off for values < 4.6566e-10, assume they are always on
// * bits 11..0 are for higher precision, they are dropped leaving only 11 bits
//     of precision
//
// When converting to compressed format, bit 11 is added to in order to make it
// a rounding rather than truncation.
//
// Out of 65556 possible values, 2047 are outside of [0,1] interval (they are in
// interval (1,2)). This is fine because the values in [0,1] are skewed towards
// 0, which is also exactly how the components of policy tend to behave (since
// they add up to 1).

// If the two assumed-on exponent bits (3<<28) are in fact off, the input is
// rounded up to the smallest value with them on. We accomplish this by
// subtracting the two bits from the input and checking for a negative result
// (the subtraction works despite crossing from exponent to significand). This
// is combined with the round-to-nearest addition (1<<11) into one op.
void Edge::SetP(float p) {
  assert(0.0f <= p && p <= 1.0f);
  constexpr int32_t roundings = (1 << 11) - (3 << 28);
  int32_t tmp;
  std::memcpy(&tmp, &p, sizeof(float));
  tmp += roundings;
  p_ = (tmp < 0) ? 0 : static_cast<uint16_t>(tmp >> 12);
}

float Edge::GetP() const {
  // Reshift into place and set the assumed-set exponent bits.
  uint32_t tmp = (static_cast<uint32_t>(p_) << 12) | (3 << 28);
  float ret;
  std::memcpy(&ret, &tmp, sizeof(uint32_t));
  return ret;
}

std::string Edge::DebugString() const {
  std::ostringstream oss;
  oss << "Move: " << move_.as_string() << " p_: " << p_ << " GetP: " << GetP();
  return oss.str();
}

std::unique_ptr<Edge[]> Edge::FromMovelist(const MoveList& moves) {
  std::unique_ptr<Edge[]> edges = std::make_unique<Edge[]>(moves.size());
  auto* edge = edges.get();
  for (const auto move : moves) edge++->move_ = move;
  return edges;
}

/////////////////////////////////////////////////////////////////////////
// Node
/////////////////////////////////////////////////////////////////////////

Node* Node::CreateSingleChildNode(Move move) {
  assert(!edges_);
  assert(!child_);
  edges_ = Edge::FromMovelist({move});
  num_edges_ = 1;
  child_ = std::make_unique<Node>(this, 0);
  return child_.get();
}

void Node::CreateEdges(const MoveList& moves) {
  assert(!edges_);
  assert(!child_);
  if (!edges_) {
    edges_ = Edge::FromMovelist(moves);
    num_edges_ = moves.size();
  }
}

Node::ConstIterator Node::Edges() const {
  return {*this, !solid_children_ ? &child_ : nullptr};
}
Node::Iterator Node::Edges() {
  return {*this, !solid_children_ ? &child_ : nullptr};
}

float Node::GetVisitedPolicy() const { return visited_policy_; }

Edge* Node::GetEdgeToNode(const Node* node) const {
  assert(node->parent_ == this);
  assert(node->index_ < num_edges_);
  return &edges_[node->index_];
}

Edge* Node::GetOwnEdge() const { return GetParent()->GetEdgeToNode(this); }

std::string Node::DebugString() const {
  std::ostringstream oss;
  oss << " Term:" << static_cast<int>(terminal_type_) << " This:" << this
      << " Parent:" << parent_ << " Index:" << index_
      << " Child:" << child_.get() << " Sibling:" << sibling_.get()
      << " WL:" << wl_ << " N:" << n_ << " N_:" << n_in_flight_
      << " Edges:" << static_cast<int>(num_edges_)
      << " Bounds:" << static_cast<int>(lower_bound_) - 2 << ","
      << static_cast<int>(upper_bound_) - 2
      << " Solid:" << solid_children_;
  return oss.str();
}

bool Node::MakeSolid() {
  if (solid_children_ || num_edges_ == 0 || IsTerminal()) return false;
  // Can only make solid if no immediate leaf childredn are in flight since we
  // allow the search code to hold references to leaf nodes across locks.
  Node* old_child_to_check = child_.get();
  uint32_t total_in_flight = 0;
  while (old_child_to_check != nullptr) {
    if (old_child_to_check->GetN() <= 1 &&
        old_child_to_check->GetNInFlight() > 0) {
      return false;
    }
    if (old_child_to_check->IsTerminal() &&
        old_child_to_check->GetNInFlight() > 0) {
      return false;
    }
    total_in_flight += old_child_to_check->GetNInFlight();
    old_child_to_check = old_child_to_check->sibling_.get();
  }
  // If the total of children in flight is not the same as self, then there are
  // collisions against immediate children (which don't update the GetNInFlight
  // of the leaf) and its not safe.
  if (total_in_flight != GetNInFlight()) {
    return false;
  }
  std::allocator<Node> alloc;
  auto* new_children = alloc.allocate(num_edges_);
  for (int i = 0; i < num_edges_; i++) {
    new (&(new_children[i])) Node(this, i);
  }
  std::unique_ptr<Node> old_child = std::move(child_);
  while (old_child) {
    int index = old_child->index_;
    new_children[index] = std::move(*old_child.get());
    // This isn't needed, but it helps crash things faster if something has gone wrong.
    old_child->parent_ = nullptr;
    gNodeGc.AddToGcQueue(std::move(old_child));
    new_children[index].UpdateChildrenParents();
    old_child = std::move(new_children[index].sibling_);
  }
  // This is a hack.
  child_ = std::unique_ptr<Node>(new_children);
  best_child_cached_ = nullptr;
  solid_children_ = true;
  return true;
}

void Node::SortEdges() {
  assert(edges_);
  assert(!child_);
  // Sorting on raw p_ is the same as sorting on GetP() as a side effect of
  // the encoding, and its noticeably faster.
  // In analyse mode it is possible to expand a node without sending it
  // to the NN first. In that case child_ already exists, and sorting edges_
  // would lead to indices being wrong.
  if (!child_) {
    std::sort(edges_.get(), (edges_.get() + num_edges_),
              [](const Edge& a, const Edge& b) { return a.p_ > b.p_; });
  }
}

void Node::MakeTerminal(GameResult result, float plies_left, Terminal type,
                        const bool inflate_terminals) {
  if (type != Terminal::TwoFold) SetBounds(result, result);
  terminal_type_ = type;
  m_ = plies_left;
  if (result == GameResult::DRAW) {
    wl_ = 0.0f;
    q_betamcts_ = 0.0f;
    d_ = 1.0f;
  } else if (result == GameResult::WHITE_WON) {
    wl_ = 1.0f;
    q_betamcts_ = 1.0f;
    d_ = 0.0f;
  } else if (result == GameResult::BLACK_WON) {
    wl_ = -1.0f;
    q_betamcts_ = -1.0f;
    d_ = 0.0f;
    // Terminal losses have no uncertainty and no reason for their U value to be
    // comparable to another non-loss choice. Force this by clearing the policy.
    if (GetParent() != nullptr) GetOwnEdge()->SetP(0.0f);
  }
  // special treatment for terminal nodes, only for draws now
    if (inflate_terminals) {
      n_betamcts_ = 10.0f; // betamcts::terminal nodes get high n
      SetRBetamcts(0.1f);
    }
}

void Node::CalculateRelevanceBetamcts(const float trust, const float prior) {
  const auto winrate = (1.0f - GetQBetamcts())/2.0f;
  const auto visits = GetNBetamcts() * trust + prior;

  auto alpha = 1.0f + winrate * visits;
  auto beta = 1.0f + (1.0f - winrate) * visits;
  // Experimenting with sharp parent eval
  /* auto logit_eval_parent = log((winrate + 0.00001) / (1.0 - winrate + 0.00001));
  auto logit_var_parent = 0.0f; */
  // Parent has uncertainty as well
  auto logit_eval_parent = log(alpha / beta);
  auto logit_var_parent = 1.0f / alpha + 1.0f / beta;

  for (const auto& child : Edges()) {
    // betamcts::child Q values are flipped
    if (child.GetN() == 0) {continue;}
    const auto winrate_child = (1.0f + child.node()->GetQBetamcts())/2.0f;
    const auto visits_child = child.GetNBetamcts() * trust + prior;

    if (visits == 0.0 && visits_child == 0.0) {
      child.SetRBetamcts(1.0);
    } else {
      auto alpha_child = 1.0f + winrate_child * visits_child;
      auto beta_child = 1.0f + (1.0f - winrate_child) * visits_child;
      auto logit_eval_child = log(alpha_child / beta_child);
      auto logit_var_child = 1.0f / alpha_child + 1.0f / beta_child;

      auto child_relevance = winrate_child == 0.0 ? 0.0 :
            1.0f + FastErfLogistic( (logit_eval_child - logit_eval_parent)
            / sqrt(2.0 * (logit_var_child + logit_var_parent)));

      child.SetRBetamcts(child_relevance);
    }
  }
}

void Node::RecalculateScoreBetamcts() {
  double q_temp = 0.0f;
  // float q_temp = q_betamcts_; // evals of expanded nodes not kept
  double n_temp = 0.0f;
  double d_temp = 0.0f;
  double m_temp = 0.0f;

  uint32_t n_vanilla = 1;

  auto losing_m = 0.0f;
  auto winning_m = 1000000.0f;
  auto prefer_tb = false;
  auto lower = GameResult::BLACK_WON;
  auto upper = GameResult::BLACK_WON;
  for (const auto& child : Edges()) {
    // Copy + paste code from SearchWorker::MaybeSetBounds().
    // TODO: Ideally this would be done in one function.
    const auto [edge_lower, edge_upper] = child.GetBounds();
    lower = std::max(edge_lower, lower);
    upper = std::max(edge_upper, upper);
    // Checkmate is the best, so short-circuit.
    const auto is_tb = child.IsTbTerminal();
    if (edge_lower == GameResult::WHITE_WON && !is_tb) {
      // Track the shortest win.
      winning_m = std::min(winning_m, child.GetM(0.0f));
    } else if (edge_upper == GameResult::BLACK_WON) {
      // Track the longest loss.
      losing_m = std::max(losing_m, child.GetM(0.0f));
    }
    prefer_tb = prefer_tb || is_tb;

    // Now recalculate visits.
    n_vanilla += child.GetN();
    const double n = child.GetNBetamcts();
    const double r = child.GetRBetamcts();
    if (n > 0) {
      const auto visits_eff = r * n;
      n_temp += visits_eff;
      // Flip Q for opponent.
      q_temp += -child.node()->GetQBetamcts() * visits_eff;
      d_temp += child.GetD(0.0f) * visits_eff;
      m_temp += child.GetM(0.0f) * visits_eff;
    }
  }
  m_temp = (n_temp > 0 ? m_temp / n_temp : 0.0f);
  // If we found a directly winning move, we don't need tablebases.
  if (winning_m < 1000.0) { prefer_tb = false; }
  // If we found a node which is supposed to be terminal, we make it terminal.
  if (lower == upper && n_vanilla > 1) {
    if (upper == GameResult::BLACK_WON) {
      auto m = losing_m + 1.0f;
      MakeTerminal(-upper, m,
        prefer_tb ? Node::Terminal::Tablebase : Node::Terminal::EndOfGame);
    } else if (upper == GameResult::WHITE_WON) {
      auto m = winning_m + 1.0f;
      MakeTerminal(-upper, m,
        prefer_tb ? Node::Terminal::Tablebase : Node::Terminal::EndOfGame);
    }
  } else if (n_temp > 0) {
    q_betamcts_ = q_temp / n_temp;
    n_betamcts_ = n_temp;
    d_ = d_temp / n_temp;
    m_ = m_temp + 1.0f;
  } else {
    // While this should never happen at all, it could only happen if all child
    // nodes are losses, so we assume a win without making the node terminal.
    // FIXME: Interacts badly with --betamcts-update-interval=1.
    /* q_betamcts_ = 1.0f;
    n_betamcts_ = n_vanilla;
    d_ = 0.0f; */
    // Don't change m_.
  }
  // In AnalyseMode it's possible that we have to recalculate n_ as well.
  if (n_vanilla != n_ && n_ > 0) {
    n_ = n_vanilla;
    // If we have to correct n_, visited policy might also be off.
    float visited_policy = 0.0f;
    for (const auto& child : Edges()) {
      if (child.GetN() > 0) visited_policy += child.GetP();
    }
    visited_policy_ = visited_policy;
  }
}

void Node::StabilizeScoreBetamcts(const float trust, const float prior,
  const int max_steps, const float threshold) {
  float q_init = 10.0; // just needs to be outside of [-1, 1] as we want to update
  auto q_new = GetQBetamcts();
    int steps = 0;
    // ensure convergence when updating evals
    // LOGFILE << "test: " << q_init - q_new;
    while (steps < max_steps && std::abs(q_new - q_init) > threshold) {
      if (steps == 50) {
        LOGFILE << "Repeating score update. Move stats: N_eff " << n_betamcts_
        << ", q=" << q_betamcts_;
        for (const auto& child : Edges()) {
          LOGFILE << "Child: q=" << child.GetQBetamcts(0.0) << ", n_eff=" <<
            child.GetNBetamcts() << ", r=" << child.GetRBetamcts();
        }
      }
      if (steps > 50) {
        LOGFILE << "iteration " << steps <<
            ", q_old: " << q_init << ", q_new: " << q_new << ", diff: " << q_new - q_init;
      }
    CalculateRelevanceBetamcts(trust, prior);
    RecalculateScoreBetamcts();
    q_init = q_new;
    q_new = GetQBetamcts();
    steps++;
  }
}


// Calculate LCB value for move ordering.
float Node::GetLCBBetamcts(float trust, float prior, float percentile) {
  const auto winrate = (1.0f + GetQBetamcts())/2.0f;
  const auto visits = GetNBetamcts() * trust + prior;

  auto alpha = 1.0f + winrate * visits;
  auto beta = 1.0f + (1.0f - winrate) * visits;
  auto logit_var = 1.0f / alpha + 1.0f / beta;

  return percentile < 1.0 ?
          (percentile > 0.0
            ? -1.0f + 2.0f * winrate / (winrate + (1.0 - winrate) *
                    FastPow((1.0 - percentile) / percentile,
                            std::sqrt(2.0 * logit_var)))
            : -1.0f) : 1.0f;
}

// Calculate the RENTS policies for all children.
void Node::SetPoliciesRENTS(float temp, float lambda, float cutoff_factor, float fpu) {
  std::array<float, 256> intermediate;
  int counter = 0;
  float total = 0.0;
  float policy_total = 0.0;
  float policy_threshold = 0.0;
  float parent_q = -GetQBetamcts();
  // The first edge has the highest policy by design.
  // If the first edge is a proven loss, visit all children.
  for (auto edge : Edges()) {
    if (counter == 0) { policy_threshold = cutoff_factor * edge.GetP() /
                         std::sqrt((float)GetN() + 1.0f); }
    if (edge.GetP() > policy_threshold) {
      float val = FastExp((edge.GetQBetamcts(fpu) - parent_q) / temp);
      intermediate[counter++] = val;
      total += val;
      policy_total += edge.GetP();
    }
  }
  const int n_children = counter;
  counter = 0;
  // Normalize policy values to add up to 1.0.
  const float scale = total > 0.0f ? 1.0f / total : 1.0f;
  const float scale_p = policy_total > 0.0f ? 1.0f / policy_total : 1.0f;
  if (policy_total == 0.0) { lambda = 0; }
  float default_policy = (policy_total == 0) && (total == 0) ?
                          1.0f / (float)n_children : 0.0f;
  for (auto edge : Edges()) {
    if (edge.GetP() > policy_threshold) {
      edge.edge()->SetPolicy(intermediate[counter++] * scale * (1 - lambda)
                             + edge.GetP() * scale_p * lambda);
    } else {
      edge.edge()->SetPolicy(default_policy);
    }
  }
}


void Node::MakeNotTerminal() {
  terminal_type_ = Terminal::NonTerminal;
  n_ = 0;

  // If we have edges, we've been extended (1 visit), so include children too.
  if (edges_) { /* TODO betamcts::update q_betamcts_ here ? */
    n_++;
    for (const auto& child : Edges()) {
      const auto n = child.GetN();
      if (n > 0) {
        n_ += n;
        // Flip Q for opponent.
        // Default values don't matter as n is > 0.
        wl_ += -child.GetWL(0.0f) * n;
        d_ += child.GetD(0.0f) * n;
      }
    }

    // Recompute with current eval (instead of network's) and children's eval.
    wl_ /= n_;
    d_ /= n_;
  }
}

void Node::SetBounds(GameResult lower, GameResult upper) {
  lower_bound_ = lower;
  upper_bound_ = upper;
}

bool Node::TryStartScoreUpdate() {
  if (n_ == 0 && n_in_flight_ > 0) return false;
  ++n_in_flight_;
  return true;
}

void Node::CancelScoreUpdate(int multivisit) {
  n_in_flight_ -= multivisit;
  best_child_cached_ = nullptr;
}

void Node::FinalizeScoreUpdate(float v, float d, float m, int multivisit,
    float multivisit_eff,
    const bool inflate_terminals=true, const bool full_betamcts_update=true) {
  if (IsTerminal()) {
    // terminal logic for draws only
/*    if ((q_betamcts_ == 0.0) && (inflate_terminals)) {
      n_betamcts_ += multivisit * 100;
    } else if (q_betamcts_ == 0.0) {
      n_betamcts_ += multivisit * 10;
    } else {
      n_betamcts_ += multivisit;
    } */

    // treat all terminals equally:
    // terminal node will start at 500 visits, getting +50 on every visit
    if (inflate_terminals) {
      n_betamcts_ += multivisit * 10;
    } else {
      n_betamcts_ += multivisit;
    }
  }

  // Recompute Q.
  wl_ += multivisit * (v - wl_) / (n_ + multivisit);
  d_ += multivisit * (d - d_) / (n_ + multivisit);
  m_ += multivisit * (m - m_) / (n_ + multivisit);
  // Recompute betamcts values.
  q_betamcts_ += multivisit_eff * (v - q_betamcts_) / (n_ + multivisit_eff);
  n_betamcts_ += multivisit_eff;


  // If first visit, update parent's sum of policies visited at least once.
  if (n_ == 0 && parent_ != nullptr) {
    parent_->visited_policy_ += parent_->edges_[index_].GetP();
    q_betamcts_ = v;
    n_betamcts_ = multivisit;
  }
  // Increment N.
  n_ += multivisit;
  // Decrement virtual loss.
  n_in_flight_ -= multivisit;
  // Check for full betamcts recalculation.
  if (full_betamcts_update && edges_) {
    RecalculateScoreBetamcts();
  }
  // Best child is potentially no longer valid.
  best_child_cached_ = nullptr;
}

void Node::AdjustForTerminal(float v, float d, float m, int multivisit) {
  // Recompute Q.
  wl_ += multivisit * v / n_;
  d_ += multivisit * d / n_;
  m_ += multivisit * m / n_;
  // Best child is potentially no longer valid. This shouldn't be needed since
  // AdjustForTerminal is always called immediately after FinalizeScoreUpdate,
  // but for safety in case that changes.
  best_child_cached_ = nullptr;
}

void Node::RevertTerminalVisits(float v, float d, float m, int multivisit) {
  // Compute new n_ first, as reducing a node to 0 visits is a special case.
  const int n_new = n_ - multivisit;
  if (n_new <= 0) {
    if (parent_ != nullptr) {
      // To keep consistency with FinalizeScoreUpdate() expanding a node again,
      // we need to reduce the parent's visited policy.
      parent_->visited_policy_ -= parent_->edges_[index_].GetP();
    }
    // If n_new == 0, reset all relevant values to 0.
    wl_ = 0.0;
    d_ = 1.0;
    m_ = 0.0;
    n_ = 0;
    n_betamcts_ = 0.0;
    q_betamcts_ = 0.0;
    r_betamcts_ = 1.0;
  } else {
    // Recompute Q and M.
    wl_ -= multivisit * (v - wl_) / n_new;
    d_ -= multivisit * (d - d_) / n_new;
    m_ -= multivisit * (m - m_) / n_new;
    // Decrement N.
    n_ -= multivisit;
    RecalculateScoreBetamcts();
  }
  // Best child is potentially no longer valid.
  best_child_cached_ = nullptr;
}

void Node::UpdateBestChild(const Iterator& best_edge, int visits_allowed) {
  best_child_cached_ = best_edge.node();
  // An edge can point to an unexpanded node with n==0. These nodes don't
  // increment their n_in_flight_ the same way and thus are not safe to cache.
  if (best_child_cached_ && best_child_cached_->GetN() == 0) {
    best_child_cached_ = nullptr;
  }
  best_child_cache_in_flight_limit_ = visits_allowed + n_in_flight_;
}

void Node::UpdateChildrenParents() {
  if (!solid_children_) {
    Node* cur_child = child_.get();
    while (cur_child != nullptr) {
      cur_child->parent_ = this;
      cur_child = cur_child->sibling_.get();
    }
  } else {
    Node* child_array = child_.get();
    for (int i = 0; i < num_edges_; i++) {
      child_array[i].parent_ = this;
    }
  }
}

void Node::ReleaseChildren() {
  gNodeGc.AddToGcQueue(std::move(child_), solid_children_ ? num_edges_ : 0);
}

void Node::ReleaseChildrenExceptOne(Node* node_to_save) {
  if (solid_children_) {
    auto new_child = std::make_unique<Node>(this, node_to_save->index_);
    *new_child = std::move(*node_to_save);
    gNodeGc.AddToGcQueue(std::move(child_), num_edges_);
    child_ = std::move(new_child);
    if (child_) {
      child_->UpdateChildrenParents();
    }
    solid_children_ = false;
  } else {
    // Stores node which will have to survive (or nullptr if it's not found).
    std::unique_ptr<Node> saved_node;
    // Pointer to unique_ptr, so that we could move from it.
    for (std::unique_ptr<Node>* node = &child_; *node;
         node = &(*node)->sibling_) {
      // If current node is the one that we have to save.
      if (node->get() == node_to_save) {
        // Kill all remaining siblings.
        gNodeGc.AddToGcQueue(std::move((*node)->sibling_));
        // Save the node, and take the ownership from the unique_ptr.
        saved_node = std::move(*node);
        break;
      }
    }
    // Make saved node the only child. (kills previous siblings).
    gNodeGc.AddToGcQueue(std::move(child_));
    child_ = std::move(saved_node);
  }
  if (!child_) {
    num_edges_ = 0;
    edges_.reset();  // Clear edges list.
  }
}

V5TrainingData Node::GetV5TrainingData(
    GameResult game_result, const PositionHistory& history,
    FillEmptyHistory fill_empty_history,
    pblczero::NetworkFormat::InputFormat input_format, float best_q,
    float best_d, float best_m) const {
  V5TrainingData result;

  // Set version.
  result.version = 5;
  result.input_format = input_format;

  // Populate planes.
  int transform;
  InputPlanes planes = EncodePositionForNN(input_format, history, 8,
                                           fill_empty_history, &transform);
  int plane_idx = 0;
  for (auto& plane : result.planes) {
    plane = ReverseBitsInBytes(planes[plane_idx++].mask);
  }

  // Populate probabilities.
  auto total_n = GetChildrenVisits();
  // Prevent garbage/invalid training data from being uploaded to server.
  // It's possible to have N=0 when there is only one legal move in position
  // (due to smart pruning).
  if (total_n == 0 && GetNumEdges() != 1) {
    throw Exception("Search generated invalid data!");
  }
  // Set illegal moves to have -1 probability.
  std::fill(std::begin(result.probabilities), std::end(result.probabilities),
            -1);
  // Set moves probabilities according to their relative amount of visits.
  for (const auto& child : Edges()) {
    result.probabilities[child.edge()->GetMove().as_nn_index(transform)] =
        total_n > 0 ? child.GetN() / static_cast<float>(total_n) : 1;
  }

  const auto& position = history.Last();
  const auto& castlings = position.GetBoard().castlings();
  // Populate castlings.
  // For non-frc trained nets, just send 1 like we used to.
  uint8_t queen_side = 1;
  uint8_t king_side = 1;
  // If frc trained, send the bit mask representing rook position.
  if (Is960CastlingFormat(input_format)) {
    queen_side <<= castlings.queenside_rook();
    king_side <<= castlings.kingside_rook();
  }

  result.castling_us_ooo = castlings.we_can_000() ? queen_side : 0;
  result.castling_us_oo = castlings.we_can_00() ? king_side : 0;
  result.castling_them_ooo = castlings.they_can_000() ? queen_side : 0;
  result.castling_them_oo = castlings.they_can_00() ? king_side : 0;

  // Other params.
  if (IsCanonicalFormat(input_format)) {
    result.side_to_move_or_enpassant =
        position.GetBoard().en_passant().as_int() >> 56;
    if ((transform & FlipTransform) != 0) {
      result.side_to_move_or_enpassant =
          ReverseBitsInBytes(result.side_to_move_or_enpassant);
    }
    // Send transform in deprecated move count so rescorer can reverse it to
    // calculate the actual move list from the input data.
    result.invariance_info =
        transform | (position.IsBlackToMove() ? (1u << 7) : 0u);
  } else {
    result.side_to_move_or_enpassant = position.IsBlackToMove() ? 1 : 0;
    result.invariance_info = 0;
  }
  result.rule50_count = position.GetRule50Ply();

  // Game result.
  if (game_result == GameResult::WHITE_WON) {
    result.result = position.IsBlackToMove() ? -1 : 1;
  } else if (game_result == GameResult::BLACK_WON) {
    result.result = position.IsBlackToMove() ? 1 : -1;
  } else {
    result.result = 0;
  }

  // Aggregate evaluation WL.
  result.root_q = -GetWL();
  result.best_q = best_q;

  // Draw probability of WDL head.
  result.root_d = GetD();
  result.best_d = best_d;

  result.root_m = GetM();
  result.best_m = best_m;

  // Unknown here - will be filled in once the full data has been collected.
  result.plies_left = 0;

  return result;
}

/////////////////////////////////////////////////////////////////////////
// EdgeAndNode
/////////////////////////////////////////////////////////////////////////

std::string EdgeAndNode::DebugString() const {
  if (!edge_) return "(no edge)";
  return edge_->DebugString() + " " +
         (node_ ? node_->DebugString() : "(no node)");
}

/////////////////////////////////////////////////////////////////////////
// NodeTree
/////////////////////////////////////////////////////////////////////////

void NodeTree::MakeMove(Move move, bool analyse_mode) {
  if (HeadPosition().IsBlackToMove()) move.Mirror();
  const auto& board = HeadPosition().GetBoard();
  auto legal_moves = board.GenerateLegalMoves();
  // TODO: Check whether doing this all the time and not only in analyse mode
  // slows anything down:
  // if (!current_head_->Edges()) {
  if (analyse_mode && !current_head_->Edges()) {
    current_head_->CreateEdges(legal_moves);
  }

  Node* new_head = nullptr;
  for (auto& n : current_head_->Edges()) {
    if (board.IsSameMove(n.GetMove(), move)) {
      new_head = n.GetOrSpawnNode(current_head_);
      // Ensure head is not terminal, so search can extend or visit children of
      // "terminal" positions, e.g., WDL hits, converted terminals, 3-fold draw.
      if (new_head->IsTerminal()) new_head->MakeNotTerminal();
      break;
    }
  }
  move = board.GetModernMove(move);
  if (!analyse_mode) {
    current_head_->ReleaseChildrenExceptOne(new_head);
    new_head = current_head_->child_.get();
  }
  current_head_ =
    new_head ? new_head : current_head_->CreateSingleChildNode(move);
  history_.Append(move);
}

void NodeTree::TrimTreeAtHead() {
  // If solid, this will be empty before move and will be moved back empty
  // afterwards which is fine.
  auto tmp = std::move(current_head_->sibling_);
  // Send dependent nodes for GC instead of destroying them immediately.
  current_head_->ReleaseChildren();
  *current_head_ = Node(current_head_->GetParent(), current_head_->index_);
  current_head_->sibling_ = std::move(tmp);
}

bool NodeTree::ResetToPosition(const std::string& starting_fen,
                               const std::vector<Move>& moves,
                               const bool analyse_mode) {
  ChessBoard starting_board;
  int no_capture_ply;
  int full_moves;
  starting_board.SetFromFen(starting_fen, &no_capture_ply, &full_moves);
  if (gamebegin_node_ &&
      (history_.Starting().GetBoard() != starting_board ||
       history_.Starting().GetRule50Ply() != no_capture_ply)) {
    // Completely different position.
    DeallocateTree();
  }

  if (!gamebegin_node_) {
    gamebegin_node_ = std::make_unique<Node>(nullptr, 0);
  }

  history_.Reset(starting_board, no_capture_ply,
                 full_moves * 2 - (starting_board.flipped() ? 1 : 2));

  Node* old_head = current_head_;
  current_head_ = gamebegin_node_.get();
  bool seen_old_head = (gamebegin_node_.get() == old_head);
  for (const auto& move : moves) {
    MakeMove(move, analyse_mode);
    if (old_head == current_head_) seen_old_head = true;
  }
  // Unless we are explicitly in analysie mode, we want to be conservative
  // with keeping the old tree around because of possible inconsistencies.
  // MakeMove guarantees that no siblings exist; but, if we didn't see the old
  // head, it means we might have a position that was an ancestor to a
  // previously searched position, which means that the current_head_ might
  // retain old n_ and q_ (etc) data, even though its old children were
  // previously trimmed; we need to reset current_head_ in that case.
  if (!seen_old_head && !analyse_mode) TrimTreeAtHead();
  return seen_old_head;
}

void NodeTree::DeallocateTree() {
  // Same as gamebegin_node_.reset(), but actual deallocation will happen in
  // GC thread.
  gNodeGc.AddToGcQueue(std::move(gamebegin_node_));
  gamebegin_node_ = nullptr;
  current_head_ = nullptr;
}

}  // namespace lczero
