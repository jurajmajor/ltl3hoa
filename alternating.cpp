/*
    Copyright (c) 2016 Juraj Major

    This file is part of LTL3TELA.

    LTL3TELA is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    LTL3TELA is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LTL3TELA.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "utils.hpp"
#include "alternating.hpp"

unsigned get_max_u_disj_size(spot::formula f) {
	if (f.is_boolean()) {
		return 1;
	}
	unsigned max = 1;
	if (f.is(spot::op::U) && f[0].is_boolean() && !f[1].is_boolean()) {
		max = f_bar(f[1]).size();
	}
	for (auto g : f) {
		auto mm = get_max_u_disj_size(g);
		if (mm > max) {
			max = mm;
		}
	}
	return max;
}

unsigned max_u_disj_size;

bool is_mergeable(SLAA* slaa, spot::formula f) {
	if (!f.is(spot::op::U)) {
		throw "Argument of is_mergeable is not an U-formula";
	}

	// we are not interested in cases where we save nothing
	if (f[1].is_boolean()) {
		return false;
	}

	// left argument has to be a state formula
	if (!f[0].is_boolean()) {
		return false;
	}

	// bdd of the left argument
	auto alpha = spot::formula_to_bdd(f[0], slaa->spot_bdd_dict, slaa->spot_aut);
	bool at_least_one_loop = false;
	// for each conjunction in DNF of psi test whether loops are covered by alpha
	for (auto& clause : f_bar(f[1])) {
		// convert a set of formulae into their conjunction
		auto sf = spot::formula::And(std::vector<spot::formula>(clause.begin(), clause.end()));
		// create the state for the conjunction
		unsigned state_id = make_alternating_recursive(slaa, sf);
		// Check that any loop label impies alpha(f[0])
		for (auto& edge_id : slaa->get_state_edges(state_id)) {
			auto t = slaa->get_edge(edge_id);
			//check t is a loop
			auto tar_states = t->get_targets();
			std::set<spot::formula> targets;
			for (auto& tar_state : tar_states) {
				targets.insert(slaa->state_name(tar_state));
			}
			if (std::includes(targets.begin(), targets.end(), clause.begin(), clause.end())) {
				// If label does not satisfy alpha, return false
				at_least_one_loop = true;
				if ((t->get_label() & alpha) != t->get_label()) {
					return false;
				}
			}
		}
	}

	if (o_mergeable_info == 1 && at_least_one_loop) {
		// only for experiments purposes
		std::cout << true << std::endl;
		std::exit(0);
	}

	return true;
}

void register_ap_from_boolean_formula(SLAA* slaa, spot::formula f) {
	// recursively register APs from a state formula f
	if (f.is(spot::op::And) || f.is(spot::op::Or)) {
		for (unsigned i = 0, size = f.size(); i < size; ++i) {
			register_ap_from_boolean_formula(slaa, f[i]);
		}
	} else {
		slaa->spot_aut->register_ap((f.is(spot::op::Not) ? spot::formula::Not(f) : f).ap_name());
	}
}

unsigned make_alternating_recursive(SLAA* slaa, spot::formula f) {
	if (slaa->state_exists(f)) {
		// we already have a state for f
		return slaa->get_state_id(f);
	} else {
		// create a new state
		unsigned state_id = slaa->get_state_id(f);

		if (f.is_tt()) {
			slaa->add_edge(state_id, bdd_true(), std::set<unsigned>());
		} else if (f.is_ff()) {
			// NOP
		} else if (f.is_boolean()) {
			// register APs in f
			register_ap_from_boolean_formula(slaa, f);

			// add the only edge to nowhere
			slaa->add_edge(state_id, spot::formula_to_bdd(f, slaa->spot_bdd_dict, slaa->spot_aut), std::set<unsigned>());
		} else if (f.is(spot::op::And)) {
			std::set<std::set<unsigned>> conj_edges;
			// create a state for each conjunct
			for (unsigned i = 0, size = f.size(); i < size; ++i) {
				conj_edges.insert(slaa->get_state_edges(make_alternating_recursive(slaa, f[i])));
			}
			// and add the product edges
			auto product = slaa->product(conj_edges, true);
			for (auto& edge : product) {
				slaa->add_edge(state_id, edge);
			}
		} else if (f.is(spot::op::Or)) {
			// create a state for each disjunct
			bdd state_labels_disj = bddfalse;
			bool same_labels = true;
			bool loops_not_alternating = true;
			auto f_bar_size = f.size();
			std::vector<std::set<unsigned>> edges_to_add(f_bar_size);
			std::vector<unsigned> fi_states;

			for (unsigned i = 0; i < f_bar_size; ++i) {
				auto fi_st_id = make_alternating_recursive(slaa, f[i]);
				auto fi_state_edges = slaa->get_state_edges(fi_st_id);

				fi_states.push_back(fi_st_id);

				bdd this_state_labels = bddfalse;
				if (o_disj_merging && same_labels) {
					for (auto edge_id : fi_state_edges) {
						auto targets = slaa->get_edge(edge_id)->get_targets();
						if (std::find(std::begin(targets), std::end(targets), fi_st_id) != std::end(targets)) {
							this_state_labels = bdd_or(this_state_labels, slaa->get_edge(edge_id)->get_label());
							if (targets.size() > 1) {
								loops_not_alternating = false;
								break;
							}
						}
					}

					if (i == 0) {
						state_labels_disj = this_state_labels;
					} else {
						same_labels = bdd_implies(state_labels_disj, this_state_labels) && bdd_implies(this_state_labels, state_labels_disj);
					}
				}

				for (auto edge_id : fi_state_edges) {
					edges_to_add[i].insert(edge_id);
				}
			}

			if (o_disj_merging && o_g_merge_level && same_labels && loops_not_alternating) {
				auto& ac = slaa->spot_aut->acc();
				// FIXME we don't have support for ignoring this yet
				// now just create a Fin mark and don't add it anywhere
				slaa->acc[f].fin = ac.add_set();
				slaa->acc[f].inf = -1U;

				unsigned min_disj_mark = ac.add_sets(f_bar_size);
				for (unsigned i = min_disj_mark; i < min_disj_mark + f_bar_size; ++i) {
					slaa->acc[f].fin_disj.insert(i);
				}

				for (unsigned i = 0; i < f_bar_size; ++i) {
					for (auto edge_id : edges_to_add[i]) {
						// is this a loop?
						auto edge = slaa->get_edge(edge_id);
						auto targets = edge->get_targets(); // copy this!
						auto fi_target_iter = std::find(std::begin(targets), std::end(targets), fi_states[i]);
						if (fi_target_iter != std::end(targets)) {
							// yes: remove this from targets set and add the whole disjunction instead
							targets.erase(fi_target_iter);
							targets.insert(state_id);

							// add each mark from [min_disj_mark .. min_disj_mark + f_bar_size) except for min_disj_mark + i
							std::set<acc_mark> marks;
							for (unsigned j = min_disj_mark; j < min_disj_mark + f_bar_size; ++j) {
								if (j != min_disj_mark + i) {
									marks.insert(j);
								}
							}

							auto orig_marks = edge->get_marks();
							marks.insert(std::begin(orig_marks), std::end(orig_marks));

							slaa->add_edge(state_id, slaa->get_edge(edge_id)->get_label(), targets, marks);
						} else {
							// just copy the edge
							slaa->add_edge(state_id, edge_id);
						}
					}
				}
			} else {
				// and add all its edges
				for (auto& edge_set : edges_to_add) {
					for (auto edge_id : edge_set) {
						slaa->add_edge(state_id, edge_id);
					}
				}
			}
		} else if (f.is(spot::op::X)) {
			if (o_x_single_succ) {
				// translate X φ as (X φ) --tt--> (φ)
				std::set<unsigned> target_set = { make_alternating_recursive(slaa, f[0]) };
				slaa->add_edge(state_id, bdd_true(), target_set);
			} else {
				// we add an universal edge to all states in each disjunct
				auto f_dnf = f_bar(f[0]);

				for (auto& g_set : f_dnf) {
					std::set<unsigned> target_set;
					for (auto& g : g_set) {
						target_set.insert(make_alternating_recursive(slaa, g));
					}
					slaa->add_edge(state_id, bdd_true(), target_set);
				}
			}

		} else if (f.is(spot::op::R)) {
			// we build automaton for f[0] even if we don't need it for G
			// however, it doesn't cost much if f[0] == ff
			// the advantage is that we don't break order of APs
			unsigned left = make_alternating_recursive(slaa, f[0]);
			unsigned right = make_alternating_recursive(slaa, f[1]);

			auto f1_dnf = f_bar(f[1]);
			if (o_g_merge_level > 0 && f[0].is_ff() && f1_dnf.size() == 1 && (o_g_merge_level == 2 || f1_dnf.begin()->size() == 1)) {
				// we have G(φ_1 & ... & φ_n) for temporal formulae φ_i
				slaa->register_dom_states(state_id, right, 2);

				auto f1_conjuncts = *(f1_dnf.begin());

				std::set<std::set<unsigned>> edges_for_product;

				for (auto& phi : f1_conjuncts) {
					unsigned phi_state = make_alternating_recursive(slaa, phi);
					slaa->register_dom_states(state_id, phi_state, 2);
					std::set<unsigned> phi_edges;

					if (phi.is(spot::op::U)) {
						// φ is U subformula
						// copy each edge for φ
						// each edge that is not a loop gets an Inf mark
						acc_mark inf = slaa->acc[phi].inf;

						for (auto& edge_id : slaa->get_state_edges(phi_state)) {
							auto edge = slaa->get_edge(edge_id);
							auto targets = edge->get_targets();
							auto marks = edge->get_marks();

							if (targets.count(phi_state) > 0) {
								// this is a loop
								targets.erase(phi_state);

								if (o_mergeable_info == 2) {
									// only for experiments purposes
									std::cout << true << std::endl;
									std::exit(0);
								}
							} else {
								// this is not a loop, add the Inf mark
								if (inf == -1U) {
									// we don't have an mark for Inf, create one
									auto& ac = slaa->spot_aut->acc();
									inf = ac.add_set();
									slaa->acc[phi].inf = inf;

									slaa->remember_inf_mark(inf);
								}

								marks = { inf };
							}

							// each copied edge should loop
							targets.insert(state_id);

							auto new_edge_id = slaa->create_edge(edge->get_label());
							auto new_edge = slaa->get_edge(new_edge_id);
							new_edge->add_target(targets);
							new_edge->add_mark(marks);

							phi_edges.insert(new_edge_id);
						}
					} else {
						// each edge goes to Gφ; transition to φ (if any) is removed
						for (auto& edge_id : slaa->get_state_edges(phi_state)) {
							auto edge = slaa->get_edge(edge_id);
							auto targets = edge->get_targets();

							if (o_mergeable_info == 2 && targets.count(phi_state) > 0) {
								// only for experiments purposes
								std::cout << true << std::endl;
								std::exit(0);
							}

							targets.erase(phi_state);
							targets.insert(state_id);

							auto new_edge_id = slaa->create_edge(edge->get_label());
							auto new_edge = slaa->get_edge(new_edge_id);
							new_edge->add_target(targets);
							new_edge->add_mark(edge->get_marks());

							phi_edges.insert(new_edge_id);
						}
					}

					edges_for_product.insert(phi_edges);
				}

				// create a product of all new edges
				auto phi_product = slaa->product(edges_for_product, true);
				for (auto edge_id : phi_product) {
					slaa->add_edge(state_id, edge_id);
				}
			} else {
				// use traditional construction
				std::set<unsigned> left_edges = slaa->get_state_edges(left);
				std::set<unsigned> right_edges = slaa->get_state_edges(right);

				unsigned loop_id = slaa->create_edge(bdd_true());
				slaa->get_edge(loop_id)->add_target(state_id);

				// remember the mark-discarding product should be used
				for (auto& right_edge : right_edges) {
					for (auto& left_edge : left_edges) {
						slaa->add_edge(state_id, slaa->edge_product(right_edge, left_edge, false));
					}
					slaa->add_edge(state_id, slaa->edge_product(right_edge, loop_id, false));
				}
			}
		} else if (f.is(spot::op::U)) {
			auto& ac = slaa->spot_aut->acc();
			acc_mark m_fin, m_inf;

			bool acc_empty = slaa->acc.empty();

			if (o_g_merge_level) {
				m_fin = slaa->acc[f].fin = ac.add_set();
				m_inf = slaa->acc[f].inf = -1U; // default value for Inf-mark, meaning the mark does not have a value
			} else {
				if (acc_empty) {
					auto _x = ac.add_set();
					if (_x != 0) {
						throw "Global Fin mark is expected to be 0.";
					}
					slaa->acc[f].fin = 0;
					slaa->acc[f].inf = -1U;
				}
				m_fin = 0;
				m_inf = -1U;
			}

			unsigned left = make_alternating_recursive(slaa, f[0]);
			unsigned right = make_alternating_recursive(slaa, f[1]);

			if (o_u_merge_level > 0 && is_mergeable(slaa, f)) {
				// we always have a loop with the Fin-mark
				slaa->add_edge(
					state_id,
					spot::formula_to_bdd(f[0], slaa->spot_bdd_dict, slaa->spot_aut),
					std::set<unsigned>({ state_id }),
					std::set<unsigned>({ m_fin })
				);
				slaa->register_dom_states(right, state_id, 1);

				// if f is a disjunction of at least two subformulae, create marks for each of these
				auto f_dnf = f_bar(f[1]);
				unsigned mark = -1U;
				unsigned f_dnf_size = f_dnf.size();
				if (f_dnf_size > 1) {
					// we add fin_disj marks only if at least two automata for the disjuncts contain a loop
					unsigned states_with_loop = 0;
					for (auto& m : f_dnf) {
						auto product_state = make_alternating_recursive(
							slaa,
							spot::formula::And(std::vector<spot::formula>(m.begin(), m.end()))
						);
						slaa->register_dom_states(product_state, state_id, 1);
						auto product_edges = slaa->get_state_edges(product_state);
						std::set<unsigned> m_state_ids;
						for (auto& m_formula : m) {
							m_state_ids.insert(make_alternating_recursive(slaa, m_formula));
						}

						bool merge = true;
						if (o_u_merge_level == 3) {
							// we won't merge if there is a looping alternating transition
							for (auto& edge_id : product_edges) {
								auto edge_targets = slaa->get_edge(edge_id)->get_targets();

								if (edge_targets.count(product_state) > 0 && edge_targets.size() >= 2) {
									merge = false;
									break;
								}
							}
						}

						for (auto& edge_id : product_edges) {
							auto edge = slaa->get_edge(edge_id);
							auto p = edge->get_targets();
							if (o_u_merge_level == 1 && p == m_state_ids || o_u_merge_level >= 2 && merge && std::includes(p.begin(), p.end(), m_state_ids.begin(), m_state_ids.end())) {
								++states_with_loop;
								break;
							}
						}
					}

					if (states_with_loop > 1) {
						if (o_g_merge_level) {
							mark = ac.add_sets(f_dnf_size);
						} else {
							if (ac.num_sets() == 1) {
								auto _x = ac.add_sets(max_u_disj_size);
								if (_x != 1) {
									throw "Disjunction Fin marks are expected to start at 1.";
								}
							}
							mark = 1;
						}

						for (acc_mark i = mark; i < mark + (o_g_merge_level ? f_dnf_size : max_u_disj_size); ++i) {
							if (acc_empty || o_g_merge_level) {
								slaa->acc[f].fin_disj.insert(i);
							}
						}
					}
				}

				// for each M ∈ DNF of f
				unsigned m_mark = mark;
				for (auto& m : f_dnf) {
					// set of state IDs of M
					std::set<unsigned> m_state_ids;
					for (auto& m_formula : m) {
						m_state_ids.insert(make_alternating_recursive(slaa, m_formula));
					}

					// build a state for product of M (equal to conjunction of M)
					auto product_state = make_alternating_recursive(
						slaa,
						spot::formula::And(std::vector<spot::formula>(m.begin(), m.end()))
					);
					auto product_edges = slaa->get_state_edges(product_state);

					bool merge = true;
					if (o_u_merge_level == 3) {
						// we won't merge if there is a looping alternating transition
						for (auto& edge_id : product_edges) {
							auto edge_targets = slaa->get_edge(edge_id)->get_targets();
								if (edge_targets.count(product_state) > 0 && edge_targets.size() >= 2) {
								merge = false;
								break;
							}
						}
					}

					// for each edge of the product
					for (auto& edge_id : product_edges) {
						auto edge = slaa->get_edge(edge_id);

						auto p = edge->get_targets();
						// does M ⊆ P hold?
						if ((o_u_merge_level == 1 && p == m_state_ids) || (o_u_merge_level >= 2 && merge && std::includes(p.begin(), p.end(), m_state_ids.begin(), m_state_ids.end()))) {
							// yes so new target set is P ∖ M plus loop to our state
							std::set<unsigned> new_edge_targets;
							auto net_it = std::set_difference(
								p.begin(), p.end(),
								m_state_ids.begin(), m_state_ids.end(),
								std::inserter(new_edge_targets, new_edge_targets.begin())
							);
							new_edge_targets.insert(state_id);

							auto new_edge_marks = edge->get_marks();
							if (mark != -1U) {
								for (unsigned i = mark; i < mark + (o_g_merge_level ? f_dnf_size : max_u_disj_size); ++i) {
									if (i != m_mark) {
										new_edge_marks.insert(i);
									}
								}
							}

							slaa->add_edge(state_id, slaa->get_edge(edge_id)->get_label(), new_edge_targets, new_edge_marks);
						} else {
							// M ⊆ P does not hold
							slaa->add_edge(state_id, edge->get_label(), edge->get_targets());
						}
					}

					++m_mark;
				}
			} else {
				// the classical construction for U
				std::set<unsigned> left_edges = slaa->get_state_edges(left);
				std::set<unsigned> right_edges = slaa->get_state_edges(right);

				unsigned loop_id = slaa->create_edge(bdd_true());
				slaa->get_edge(loop_id)->add_target(state_id);

				slaa->add_edge(state_id, right_edges);

				acc_mark fin;
				if (o_g_merge_level) {
					fin = slaa->acc[f].fin;
				} else {
					fin = std::begin(slaa->acc)->second.fin;
				}

				for (auto& left_edge : left_edges) {
					auto p = slaa->edge_product(left_edge, loop_id, true);
					// the only mark is the new Fin
					slaa->get_edge(p)->clear_marks();
					slaa->get_edge(p)->add_mark(fin);
					slaa->add_edge(state_id, p);
				}
			}
		}

		return state_id;
	}
}

SLAA* make_alternating(spot::formula f, spot::bdd_dict_ptr dict) {
	SLAA* slaa = new SLAA(f, dict);

	max_u_disj_size = get_max_u_disj_size(f);

	if (o_single_init_state) {
		std::set<unsigned> init_set = { make_alternating_recursive(slaa, f) };
		slaa->add_init_set(init_set);
	} else {
		std::set<std::set<spot::formula>> f_dnf = f_bar(f);

		for (auto& g_set : f_dnf) {
			std::set<unsigned> init_set;
			for (auto& g : g_set) {
				unsigned init_state_id = make_alternating_recursive(slaa, g);
				init_set.insert(init_state_id);
			}
			slaa->add_init_set(init_set);
		}
	}

	slaa->build_acc();

	if (o_slaa_determ == 2) {
		slaa->apply_extended_domination();
	}

	return slaa;
}
