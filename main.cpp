// {{{ GPL License

// This file is part of gringo - a grounder for logic programs.
// Copyright Roland Kaminski

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// }}}

#include <clingo.hh>
#include <iostream>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <vector>
#include <queue>
#include <limits>
#include <chrono>
#include <iomanip>

using namespace Clingo;

namespace Detail {

template <int X>
using int_type = std::integral_constant<int, X>;
template <class T, class S>
inline void nc_check(S s, int_type<0>) { // same sign
    (void)s;
    assert((std::is_same<T, S>::value) || (s >= std::numeric_limits<T>::min() && s <= std::numeric_limits<T>::max()));
}
template <class T, class S>
inline void nc_check(S s, int_type<-1>) { // Signed -> Unsigned
    (void)s;
    assert(s >= 0 && static_cast<S>(static_cast<T>(s)) == s);
}
template <class T, class S>
inline void nc_check(S s, int_type<1>) { // Unsigned -> Signed
    (void)s;
    assert(!(s > std::numeric_limits<T>::max()));
}

} // namespace Detail

template <class T, class S>
inline T numeric_cast(S s) {
    constexpr int sv = int(std::numeric_limits<T>::is_signed) - int(std::numeric_limits<S>::is_signed);
    ::Detail::nc_check<T>(s, ::Detail::int_type<sv>());
    return static_cast<T>(s);
}

struct Edge {
    int from;
    int to;
    int weight;
    literal_t lit;
};

template <class K, class V>
std::ostream &operator<<(std::ostream &out, std::unordered_map<K, V> const &map);
template <class T>
std::ostream &operator<<(std::ostream &out, std::vector<T> const &vec);
template <class K, class V>
std::ostream &operator<<(std::ostream &out, std::pair<K, V> const &pair);

template <class T>
std::ostream &operator<<(std::ostream &out, std::vector<T> const &vec) {
    out << "{";
    for (auto &x : vec) {
        out << " " << x;
    }
    out << " }";
    return out;
}

template <class K, class V>
std::ostream &operator<<(std::ostream &out, std::unordered_map<K, V> const &map) {
    using T = std::pair<K, V>;
    std::vector<T> vec;
    vec.assign(map.begin(), map.end());
    std::sort(vec.begin(), vec.end(), [](T const &a, T const &b) { return a.first < b.first; });
    out << vec;
    return out;
}

template <class K, class V>
std::ostream &operator<<(std::ostream &out, std::pair<K, V> const &pair) {
    out << "( " << pair.first << " " << pair.second << " )";
    return out;
}

template <class C>
void ensure_index(C &c, size_t index) {
    if (index >= c.size()) {
        c.resize(index + 1);
    }
}

using Duration = std::chrono::duration<double>;

class Timer {
public:
    Timer(Duration &elapsed)
        : elapsed_(elapsed)
        , start_(std::chrono::steady_clock::now()) {}
    ~Timer() { elapsed_ += std::chrono::steady_clock::now() - start_; }

private:
    Duration &elapsed_;
    std::chrono::time_point<std::chrono::steady_clock> start_;
};

constexpr int undefined_potential = std::numeric_limits<int>::min();
struct DifferenceLogicNode {
    std::vector<int> outgoing;
    int potential = undefined_potential;
    int last_edge = 0;
    int gamma = 0;
    bool changed = false;
};

struct DifferenceLogicNodeUpdate {
    int node_idx;
    int gamma;
};
bool operator<(DifferenceLogicNodeUpdate const &a, DifferenceLogicNodeUpdate const &b) { return a.gamma > b.gamma; }

class DifferenceLogicGraph {
public:
    DifferenceLogicGraph(const std::vector<Edge> &edges)
        : edges_(edges) {}

    bool empty() const { return nodes_.empty(); }

    int node_value_defined(int idx) const { return nodes_[idx].potential != undefined_potential; }
    int node_value(int idx) const { return -nodes_[idx].potential; }

    std::vector<int> add_edge(int uv_idx) {
        auto &uv = edges_[uv_idx];

        // initialize the nodes of the edge to add
        ensure_index(nodes_, std::max(uv.from, uv.to));
        auto &u = nodes_[uv.from];
        auto &v = nodes_[uv.to];
        if (u.potential == undefined_potential) {
            u.potential = 0;
        }
        if (v.potential == undefined_potential) {
            v.potential = 0;
        }
        v.gamma = u.potential + uv.weight - v.potential;
        if (v.gamma < 0) {
            gamma_.push({uv.to, v.gamma});
            v.last_edge = uv_idx;
        }

        // detect negative cycles
        while (!gamma_.empty() && u.gamma == 0) {
            auto s_idx = gamma_.top().node_idx;
            auto &s = nodes_[s_idx];
            if (!s.changed) {
                assert(s.gamma == gamma_.top().gamma);
                s.potential += s.gamma;
                s.gamma = 0;
                s.changed = true;
                changed_.emplace_back(s_idx);
                for (auto st_idx : s.outgoing) {
                    assert(st_idx < numeric_cast<int>(edges_.size()));
                    auto &st = edges_[st_idx];
                    auto &t = nodes_[st.to];
                    if (!t.changed) {
                        auto gamma = s.potential + st.weight - t.potential;
                        if (gamma < t.gamma) {
                            t.gamma = gamma;
                            gamma_.push({st.to, t.gamma});
                            t.last_edge = st_idx;
                        }
                    }
                }
            }
            gamma_.pop();
        }

        std::vector<int> neg_cycle;
        if (u.gamma < 0) {
            // gather the edges in the negative cycle
            neg_cycle.push_back(v.last_edge);
            auto next_idx = edges_[v.last_edge].from;
            while (uv.to != next_idx) {
                auto &next = nodes_[next_idx];
                neg_cycle.push_back(next.last_edge);
                next_idx = edges_[next.last_edge].from;
            }
        }
        else {
            // add the edge to the graph
            u.outgoing.emplace_back(uv_idx);
        }

        // reset gamma and changed flags
        v.gamma = 0;
        while (!gamma_.empty()) {
            nodes_[gamma_.top().node_idx].gamma = 0;
            gamma_.pop();
        }
        for (auto x : changed_) {
            nodes_[x].changed = false;
        }
        changed_.clear();

        return neg_cycle;
    }

    void reset() { nodes_.clear(); }

private:
    std::priority_queue<DifferenceLogicNodeUpdate> gamma_;
    std::vector<int> changed_;
    const std::vector<Edge> &edges_;
    std::vector<DifferenceLogicNode> nodes_;
};

struct DLStats {
    Duration time_propagate = Duration{0};
    Duration time_undo = Duration{0};
};

struct Stats {
    Duration time_total = Duration{0};
    Duration time_init = Duration{0};
    std::vector<DLStats> dl_stats;
};

struct DLState {
    DLState(DLStats &stats, const std::vector<Edge> &edges)
        : stats(stats)
        , dl_graph(edges) {}
    DLStats &stats;
    std::vector<int> edge_trail;
    DifferenceLogicGraph dl_graph;
    int propagated = 0;
};

class DifferenceLogicPropagator : public Propagator {
public:
    DifferenceLogicPropagator(Stats &stats)
        : stats_(stats) {}

    void print_assignment(int thread) const {
        auto &state = states_[thread];
        std::cout << "with assignment:\n";
        int idx = 0;
        for (std::string const &name : vert_map_) {
            if (state.dl_graph.node_value_defined(idx)) {
                std::cout << name << ":" << state.dl_graph.node_value(idx) << " ";
            }
            ++idx;
        }
        std::cout << "\n";
    }

private:
    // initialization

    void init(PropagateInit &init) override {
        Timer t{stats_.time_init};
        for (auto atom : init.theory_atoms()) {
            auto term = atom.term();
            if (term.to_string() == "diff") {
                add_edge_atom(init, atom);
            }
        }
        initialize_states(init);
    }

    void add_edge_atom(PropagateInit &init, TheoryAtom const &atom) {
        int lit = init.solver_literal(atom.literal());
        int weight = 0;
        if (atom.guard().second.arguments().empty()) { // Check if constant is  negated
            weight = atom.guard().second.number();
        }
        else {
            weight = -atom.guard().second.arguments()[0].number();
        }
        auto u_id = map_vert(atom.elements()[0].tuple()[0].arguments()[0].to_string());
        auto v_id = map_vert(atom.elements()[0].tuple()[0].arguments()[1].to_string());
        auto id = numeric_cast<int>(edges_.size());
        edges_.push_back({u_id, v_id, weight, lit});
        lit_to_edges_.emplace(lit, id);
        init.add_watch(lit);
    }

    int map_vert(std::string v) {
        auto ret = vert_map_inv_.emplace(std::move(v), vert_map_.size());
        if (ret.second) {
            vert_map_.emplace_back(ret.first->first);
        }
        return ret.first->second;
    }

    void initialize_states(PropagateInit &init) {
        stats_.dl_stats.resize(init.number_of_threads());
        for (int i = 0; i < init.number_of_threads(); ++i) {
            states_.emplace_back(stats_.dl_stats[i], edges_);
        }
    }

    // propagation

    void propagate(PropagateControl &ctl, LiteralSpan changes) override {
        auto &state = states_[ctl.thread_id()];
        Timer t{state.stats.time_propagate};
        for (auto lit : changes) {
            state.edge_trail.emplace_back(lit);
        }
        check_consistency(ctl, state);
    }

    bool check_consistency(PropagateControl &ctl, DLState &state) {
        for (; state.propagated < numeric_cast<int>(state.edge_trail.size()); ++state.propagated) {
            auto lit = state.edge_trail[state.propagated];
            for (auto it = lit_to_edges_.find(lit), ie = lit_to_edges_.end(); it != ie && it->first == lit; ++it) {
                auto neg_cycle = state.dl_graph.add_edge(it->second);
                if (!neg_cycle.empty()) {
                    std::vector<literal_t> clause;
                    for (auto eid : neg_cycle) {
                        clause.emplace_back(-edges_[eid].lit);
                    }
                    if (!ctl.add_clause(clause) || !ctl.propagate()) {
                        return false;
                    }
                    assert(false && "must not happen");
                }
            }
        }

        return true;
    }

    // undo

    void undo(PropagateControl const &ctl, LiteralSpan changes) override {
        auto &state = states_[ctl.thread_id()];
        Timer t{state.stats.time_undo};
        state.edge_trail.resize(state.edge_trail.size() - changes.size());
        state.propagated = 0;
        state.dl_graph.reset();
    }

private:
    std::vector<DLState> states_;
    std::unordered_multimap<literal_t, int> lit_to_edges_;
    std::vector<Edge> edges_;
    std::vector<std::reference_wrapper<const std::string>> vert_map_;
    std::unordered_map<std::string, int> vert_map_inv_;
    Stats &stats_;
};

int get_int(std::string constname, Control &ctl, int def) {
    Symbol val = ctl.get_const(constname.c_str());
    if (val.to_string() == constname.c_str()) {
        return def;
    }
    return val.number();
}

int main(int argc, char *argv[]) {
    Stats stats;
    {
        Timer t{stats.time_total};
        auto argb = argv + 1, arge = argb;
        for (; *argb; ++argb, ++arge) {
            if (std::strcmp(*argb, "--") == 0) {
                ++argb;
                break;
            }
        }
        Control ctl{{argb, numeric_cast<size_t>(argv + argc - argb)}};
        ctl.add("base", {}, R"(#theory dl {
        term{};
        constant {- : 1, unary};
        diff_term {- : 1, binary, left};
        &diff/0 : diff_term, {<=}, constant, any;
        &show_assignment/0 : term, directive
    }.)");
        for (auto arg = argv + 1; arg != arge; ++arg) {
            ctl.load(*arg);
        }
        // TODO: configure strict/non-strict mode
        // int c = get_int("strict", ctl, 0);

        DifferenceLogicPropagator p{stats};
        ctl.register_propagator(p);
        ctl.ground({{"base", {}}});
        int i = 0;
        for (auto m : ctl.solve()) {
            i++;
            std::cout << "Answer " << i << "\n";
            std::cout << m << "\n";
            p.print_assignment(m.context().thread_id());
        }
        if (i == 0) {
            std::cout << "UNSATISFIABLE\n";
        }
        else {
            std::cout << "SATISFIABLE\n";
        }
    }

    std::cout << "total: " << stats.time_total.count() << "s\n";
    std::cout << "  init: " << stats.time_init.count() << "s\n";
    int thread = 0;
    for (auto &stat : stats.dl_stats) {
        std::cout << "  total[" << thread << "]: ";
        std::cout << (stat.time_undo + stat.time_propagate).count() << "s\n";
        std::cout << "    propagate: " << stat.time_propagate.count() << "s\n";
        std::cout << "    undo     : " << stat.time_undo.count() << "s\n";
        ++thread;
    }
}
