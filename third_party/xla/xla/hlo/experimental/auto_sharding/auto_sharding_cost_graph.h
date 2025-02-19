/* Copyright 2022 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_HLO_EXPERIMENTAL_AUTO_SHARDING_AUTO_SHARDING_COST_GRAPH_H_
#define XLA_HLO_EXPERIMENTAL_AUTO_SHARDING_AUTO_SHARDING_COST_GRAPH_H_

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "xla/hlo/experimental/auto_sharding/auto_sharding_strategy.h"
#include "xla/hlo/experimental/auto_sharding/matrix.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/shape_util.h"

namespace xla {
namespace spmd {

// A graph data structure to simplify the edge cost graph. It merges nodes and
// performs path compression.
class CostGraph {
 public:
  CostGraph(const StrategyGroups& strategy_groups,
            const AssociativeDotPairs& associative_dot_pairs) {
    node_lens_.reserve(strategy_groups.size());
    extra_node_costs_.reserve(strategy_groups.size());
    adjacency_.assign(strategy_groups.size(), StableHashSet<int>());

    // Build the cost graph.
    for (StrategyGroup* strategy_group : strategy_groups) {
      node_lens_.push_back(strategy_group->strategies.size());
      extra_node_costs_.push_back(
          std::vector<double>(strategy_group->strategies.size(), 0.0));

      const auto& in_nodes = strategy_group->in_nodes;
      for (size_t i = 0; i < in_nodes.size(); ++i) {
        if (!in_nodes[i]->is_tuple) {
          NodeIdx src_idx = in_nodes[i]->node_idx;
          NodeIdx dst_idx = strategy_group->node_idx;
          Matrix edge_communication_cost =
              CreateEdgeCommunicationCost(src_idx, dst_idx, i, strategy_group);
          Matrix edge_memory_cost =
              CreateEdgeMemoryCost(src_idx, dst_idx, i, strategy_group);
          AddEdgeCost(src_idx, dst_idx, edge_communication_cost,
                      edge_memory_cost);
        } else if (in_nodes[i]->is_tuple && in_nodes.size() > 1) {
          for (size_t l = 0; l < in_nodes[i]->childs.size(); ++l) {
            NodeIdx src_idx = in_nodes[i]->childs[l]->node_idx;
            NodeIdx dst_idx = strategy_group->node_idx;
            Matrix edge_communication_cost = CreateEdgeCommunicationCost(
                src_idx, dst_idx, i, strategy_group, true);
            Matrix edge_memory_cost =
                CreateEdgeMemoryCost(src_idx, dst_idx, i, strategy_group, true);
            AddEdgeCost(src_idx, dst_idx, edge_communication_cost,
                        edge_memory_cost);
          }
        } else {
          CHECK_EQ(in_nodes.size(), 1)
              << "Do not support instructions with more than one tuple "
                 "operand. If this CHECK fails, we will need to fix "
                 "b/233412625.";
          for (size_t l = 0; l < in_nodes[i]->childs.size(); ++l) {
            NodeIdx src_idx = in_nodes[i]->childs[l]->node_idx;
            NodeIdx dst_idx = strategy_group->node_idx;
            // TODO(b/233412625) Support more general case, e.g., multiple tuple
            // operands. If there is only one operand and it's a tuple, the
            // first index of communication_resharding_costs is for the tuple
            // element.
            Matrix edge_communication_cost = CreateEdgeCommunicationCost(
                src_idx, dst_idx, /*in_node_idx=*/l, strategy_group);
            Matrix edge_memory_cost = CreateEdgeMemoryCost(
                src_idx, dst_idx, /*in_node_idx=*/l, strategy_group);
            AddEdgeCost(src_idx, dst_idx, edge_communication_cost,
                        edge_memory_cost);
          }
        }
      }

      if (strategy_group->following) {
        if (strategy_group->strategies.size() ==
            strategy_group->following->strategies.size()) {
          to_merge_pairs_.push_back(
              {strategy_group->node_idx, strategy_group->following->node_idx});
        } else {
          LOG(WARNING) << "Different strategy counts for instruction ID "
                       << strategy_group->instruction_id
                       << " and following instruction ID "
                       << strategy_group->following->instruction_id;
        }
      }
    }

    // Adjust the edge costs for dot pairs that can be optimized by
    // AllReduceReassociate.
    for (const auto& pair : associative_dot_pairs) {
      NodeIdx src_idx = pair.first->node_idx;
      NodeIdx dst_idx = pair.second->node_idx;

      Matrix edge_communication_cost(node_lens_[src_idx], node_lens_[dst_idx]);
      Matrix edge_memory_cost(node_lens_[src_idx], node_lens_[dst_idx]);
      absl::flat_hash_map<std::string, NodeStrategyIdx>
          src_strategy_name_to_idx_map;
      for (NodeStrategyIdx i = 0; i < node_lens_[src_idx]; ++i) {
        const ShardingStrategy& strategy =
            strategy_groups[src_idx]->strategies[i];
        if (strategy.communication_cost > 0) {
          src_strategy_name_to_idx_map[strategy.name] = i;
        }
      }
      for (NodeStrategyIdx i = 0; i < node_lens_[dst_idx]; ++i) {
        const ShardingStrategy& dst_strategy =
            strategy_groups[dst_idx]->strategies[i];
        if (dst_strategy.communication_cost > 0) {
          auto it = src_strategy_name_to_idx_map.find(dst_strategy.name);
          if (it != src_strategy_name_to_idx_map.end()) {
            const ShardingStrategy& src_strategy =
                strategy_groups[src_idx]->strategies[it->second];
            CHECK_LE(std::abs(src_strategy.communication_cost -
                              dst_strategy.communication_cost),
                     1e-6);
            edge_communication_cost(it->second, i) =
                -src_strategy.communication_cost;
          }
        }
      }
      AddEdgeCost(src_idx, dst_idx, edge_communication_cost, edge_memory_cost);
    }
  }

  Matrix CreateEdgeCommunicationCost(NodeIdx src_idx, NodeIdx dst_idx,
                                     size_t in_node_idx,
                                     StrategyGroup* strategy_group,
                                     bool zero_cost = false) {
    CHECK_LT(src_idx, node_lens_.size());
    CHECK_LT(dst_idx, node_lens_.size());
    Matrix edge_communication_cost(node_lens_[src_idx], node_lens_[dst_idx]);
    for (NodeStrategyIdx k = 0; k < strategy_group->strategies.size(); ++k) {
      const ShardingStrategy& strategy = strategy_group->strategies[k];
      size_t start_idx = 0;
      if (strategy.communication_resharding_costs[in_node_idx].size() >
          node_lens_[src_idx]) {
        start_idx =
            strategy.communication_resharding_costs[in_node_idx].size() -
            node_lens_[src_idx];
      }
      for (size_t j = start_idx;
           j < strategy.communication_resharding_costs[in_node_idx].size();
           ++j) {
        edge_communication_cost(j - start_idx, k) =
            zero_cost ? 0
                      : strategy.communication_resharding_costs[in_node_idx][j];
      }
    }
    return edge_communication_cost;
  }

  Matrix CreateEdgeMemoryCost(NodeIdx src_idx, NodeIdx dst_idx,
                              size_t in_node_idx, StrategyGroup* strategy_group,
                              bool zero_cost = false) {
    CHECK_LT(src_idx, node_lens_.size());
    CHECK_LT(dst_idx, node_lens_.size());
    Matrix edge_communication_cost(node_lens_[src_idx], node_lens_[dst_idx]);
    for (NodeStrategyIdx k = 0; k < strategy_group->strategies.size(); ++k) {
      const ShardingStrategy& strategy = strategy_group->strategies[k];
      size_t start_idx = 0;
      CHECK_LT(in_node_idx, strategy.memory_resharding_costs.size())
          << strategy_group->node_idx;
      if (strategy.memory_resharding_costs[in_node_idx].size() >
          node_lens_[src_idx]) {
        start_idx = strategy.memory_resharding_costs[in_node_idx].size() -
                    node_lens_[src_idx];
      }
      for (size_t j = start_idx;
           j < strategy.memory_resharding_costs[in_node_idx].size(); ++j) {
        edge_communication_cost(j - start_idx, k) =
            zero_cost ? 0 : strategy.memory_resharding_costs[in_node_idx][j];
      }
    }
    return edge_communication_cost;
  }

  Matrix GetEdgeCommunicationCost(NodeIdx i, NodeIdx j) {
    if (i <= j) {
      return edge_communication_costs_[{i, j}];
    }
    return edge_communication_costs_[{j, i}].Transpose();
  }

  Matrix GetEdgeMemoryCost(NodeIdx i, NodeIdx j) {
    if (i <= j) {
      return edge_memory_costs_[{i, j}];
    }
    return edge_memory_costs_[{j, i}].Transpose();
  }

  void AddEdgeCost(NodeIdx i, NodeIdx j, Matrix& cost, Matrix& memory_cost) {
    if (i > j) {
      std::swap(i, j);
      cost = cost.Transpose();
      memory_cost = memory_cost.Transpose();
    }

    if (edge_communication_costs_.contains({i, j})) {
      CHECK(adjacency_[i].contains(j));
      CHECK(adjacency_[j].contains(i));
      edge_communication_costs_[{i, j}] =
          edge_communication_costs_[{i, j}] + cost;
      edge_memory_costs_[{i, j}] = edge_memory_costs_[{i, j}] + memory_cost;
    } else {
      adjacency_[i].insert(j);
      adjacency_[j].insert(i);
      edge_communication_costs_[{i, j}] = cost;
      edge_memory_costs_[{i, j}] = memory_cost;
    }
  }

  void RemoveEdge(NodeIdx i, NodeIdx j) {
    if (i > j) {
      std::swap(i, j);
    }

    CHECK(adjacency_[i].contains(j));
    CHECK(adjacency_[j].contains(i));
    CHECK(edge_communication_costs_.contains({i, j}));
    CHECK(edge_memory_costs_.contains({i, j}));

    adjacency_[i].erase(j);
    adjacency_[j].erase(i);
    edge_communication_costs_.erase({i, j});
    edge_memory_costs_.erase({i, j});
  }

  void MergeNode(NodeIdx src, NodeIdx dst) {
    // Merge node src into node dst. This is used when we set one operator to
    // follow another operator's sharding spec. For the following computation
    // graph:
    //   dst -- src -- adj1
    //           |
    //          adj2
    //
    // It will be transformed into the following graph:
    //   (src)
    //    dst -- adj1
    //     |
    //    adj2
    // Where all the edges costs between src and adjs will be added into
    // the edge costs between dst and adjs. The edge cost between src and
    // dst will be added to the extra node cost of dst. Other node costs of
    // src will be added into dst's node cost in the ILP.

    CHECK(adjacency_[src].contains(dst));
    CHECK(adjacency_[dst].contains(src));
    CHECK(!merged_to_.contains(src));
    CHECK(!merged_to_.contains(dst));
    CHECK_NE(src, dst);

    Matrix edge_communication_cost = GetEdgeCommunicationCost(dst, src);

    std::vector<NodeStrategyIdx> reindexing(node_lens_[dst]);
    if (node_lens_[dst] == node_lens_[src]) {
      // Assume the orders of strategies in src and dst match
      // (i.e., i-th strategy in src follows i-th strategy in dst).
      // This is true in most cases because of how we create the
      // following strategies.
      std::iota(reindexing.begin(), reindexing.end(), 0);
    } else {
      // Otherwise, find the strategy to follow greedily.
      // For every strategy in dst, find the strategy in src with
      // the lowest resharding cost.
      std::vector<int> arange(node_lens_[src]);
      std::iota(arange.begin(), arange.end(), 0);
      for (NodeStrategyIdx i = 0; i < node_lens_[dst]; ++i) {
        std::vector<std::pair<double, int>> keys;

        // If there are multiple strategies with the same lowest costs,
        // prefer to follow "replicated", which has the largest index.
        // Node: We assume the strategy "Repilcated" is always appended
        // as the last strategy in BuildStrategyAndCost.
        keys.reserve(node_lens_[src]);
        for (NodeStrategyIdx j = 0; j < node_lens_[src]; ++j) {
          keys.push_back({edge_communication_cost(i, j), -j});
        }

        std::sort(arange.begin(), arange.end(), [&keys](int l, int r) {
          return (keys[l].first < keys[r].first) ||
                 (keys[l].first == keys[r].first &&
                  keys[l].second < keys[r].second);
        });
        reindexing[i] = arange.front();
      }
    }
    merged_to_[src] = dst;
    reindexing_vector_[src] = reindexing;

    // Merge edge-cost matrix.
    std::vector<NodeIdx> adj_list(adjacency_[src].begin(),
                                  adjacency_[src].end());
    for (const NodeIdx adj : adj_list) {
      if (adj == dst) {
        for (NodeStrategyIdx i = 0; i < node_lens_[dst]; ++i) {
          extra_node_costs_[dst][i] +=
              edge_communication_cost(i, reindexing[i]);
        }
      } else {
        Matrix added_edge_communication_cost(node_lens_[dst], node_lens_[adj]);
        Matrix added_edge_memory_cost(node_lens_[dst], node_lens_[adj]);
        Matrix edge_communication_cost_src_adj =
            GetEdgeCommunicationCost(src, adj);
        Matrix edge_memory_cost_src_adj = GetEdgeMemoryCost(src, adj);

        for (NodeStrategyIdx i = 0; i < node_lens_[dst]; ++i) {
          for (NodeStrategyIdx k = 0; k < node_lens_[adj]; ++k) {
            added_edge_communication_cost(i, k) =
                edge_communication_cost_src_adj(reindexing[i], k);
            added_edge_memory_cost(i, k) =
                edge_memory_cost_src_adj(reindexing[i], k);
          }
        }
        AddEdgeCost(dst, adj, added_edge_communication_cost,
                    added_edge_memory_cost);
      }
    }
    // Remove edges
    for (const NodeIdx adj : adj_list) {
      RemoveEdge(src, adj);
    }
  }

  NodeIdx QueryDestination(NodeIdx node_idx) {
    if (merged_to_.contains(node_idx)) {
      NodeIdx old_dst = merged_to_[node_idx];
      NodeIdx new_dst = QueryDestination(old_dst);
      if (old_dst != new_dst) {
        // Compress path.
        absl::Span<const NodeStrategyIdx> old_reindexing_vector =
            reindexing_vector_[node_idx];
        std::vector<NodeStrategyIdx> new_reindexing_vector;
        new_reindexing_vector.reserve(node_lens_.size());
        for (NodeStrategyIdx i = 0; i < node_lens_[new_dst]; ++i) {
          new_reindexing_vector.push_back(
              old_reindexing_vector[reindexing_vector_[old_dst][i]]);
        }
        reindexing_vector_[node_idx] = new_reindexing_vector;
        merged_to_[node_idx] = new_dst;
      }
      return new_dst;
    }
    return node_idx;
  }

  void Simplify(bool enable) {
    // Merge nodes.
    if (enable) {
      for (const auto& [src, dst] : to_merge_pairs_) {
        MergeNode(src, QueryDestination(dst));
      }
    }
    // Build follow map.
    follow_idx_.reserve(node_lens_.size());
    for (NodeIdx i = 0; i < node_lens_.size(); ++i) {
      if (merged_to_.contains(i)) {
        follow_idx_.push_back(QueryDestination(i));
      } else {
        follow_idx_.push_back(-1);
      }
    }
  }

  NodeStrategyIdx RemapIndex(NodeIdx node_id, NodeStrategyIdx value) const {
    if (follow_idx_[node_id] < 0) {
      return value;
    }
    return reindexing_vector_.at(node_id)[value];
  }

  std::string ToString() {
    std::string str;
    absl::StrAppend(&str, "Cost Graph:\n");

    for (NodeIdx i = 0; i < node_lens_.size(); ++i) {
      absl::StrAppend(&str, "Node", i, ": ", node_lens_[i], "\n");
    }
    absl::StrAppend(&str, "\n");

    for (const auto& iter : edge_communication_costs_) {
      absl::StrAppend(&str, "Edge (", iter.first.first, ", ", iter.first.second,
                      "):\n");
      absl::StrAppend(&str, iter.second.ToString(), "\n");
    }

    return str;
  }

  // The number of strategies of each node.
  std::vector<int> node_lens_;
  // The adjacency list of each node.
  std::vector<StableHashSet<int>> adjacency_;
  // The cost matrix between two nodes.

  StableHashMap<std::pair<NodeIdx, NodeIdx>, Matrix> edge_communication_costs_;
  StableHashMap<std::pair<NodeIdx, NodeIdx>, Matrix> edge_memory_costs_;
  // The extra node costs introduced by merging nodes.
  std::vector<std::vector<double>> extra_node_costs_;
  // The reindexing vector of the node.
  // A reindexing vector maps a strategy index from the node being followed
  // to a strategy index of the current node.
  StableHashMap<int, std::vector<NodeStrategyIdx>> reindexing_vector_;
  // Maps a node id to the node id that is being followed by this node.
  // The value is -1 if the current node does not follow any node.
  std::vector<NodeIdx> follow_idx_;

  // Save the destination of merged nodes.
  StableHashMap<NodeIdx, NodeIdx> merged_to_;
  // Save pairs that need to be merged.
  std::vector<std::pair<NodeIdx, NodeIdx>> to_merge_pairs_;
};

// Get the final sharding strategy according to the ILP solution.
inline const ShardingStrategy& GetShardingStrategy(
    const HloInstruction* inst, const StrategyMap& strategy_map,
    const CostGraph& cost_graph, absl::Span<const NodeStrategyIdx> s_val) {
  const StrategyGroup* strategy_group = strategy_map.at(inst).get();
  CHECK(!strategy_group->is_tuple);
  NodeIdx node_idx = strategy_group->node_idx;
  NodeStrategyIdx stra_idx = cost_graph.RemapIndex(node_idx, s_val[node_idx]);
  return strategy_group->strategies[stra_idx];
}

// Get the final sharding strategy according to the ILP solution.
inline const ShardingStrategy& GetShardingStrategyForTuple(
    const HloInstruction* inst, ShapeIndex index,
    const StrategyMap& strategy_map, const CostGraph& cost_graph,
    absl::Span<const NodeStrategyIdx> s_val) {
  const StrategyGroup* strategy_group = strategy_map.at(inst).get();
  CHECK(strategy_group->is_tuple);
  for (auto index_element : index) {
    CHECK_LT(index_element, strategy_group->childs.size());
    const auto& strategies = strategy_group->childs[index_element];
    strategy_group = strategies.get();
  }
  NodeIdx node_idx = strategy_group->node_idx;
  NodeStrategyIdx stra_idx = cost_graph.RemapIndex(node_idx, s_val[node_idx]);
  return strategy_group->strategies[stra_idx];
}

}  // namespace spmd
}  // namespace xla

#endif  // XLA_HLO_EXPERIMENTAL_AUTO_SHARDING_AUTO_SHARDING_COST_GRAPH_H_
