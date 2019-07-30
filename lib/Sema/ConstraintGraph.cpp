//===--- ConstraintGraph.cpp - Constraint Graph ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c ConstraintGraph class, which describes the
// relationships among the type variables within a constraint system.
//
//===----------------------------------------------------------------------===//

#include "ConstraintGraph.h"
#include "ConstraintGraphScope.h"
#include "ConstraintSystem.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <memory>
#include <numeric>

using namespace swift;
using namespace constraints;

#define DEBUG_TYPE "ConstraintGraph"

#pragma mark Graph construction/destruction

ConstraintGraph::ConstraintGraph(ConstraintSystem &cs) : CS(cs) { }

ConstraintGraph::~ConstraintGraph() {
  assert(Changes.empty() && "Scope stack corrupted");
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto &impl = TypeVariables[i]->getImpl();
    delete impl.getGraphNode();
    impl.setGraphNode(nullptr);
  }
}

#pragma mark Graph accessors

std::pair<ConstraintGraphNode &, unsigned>
ConstraintGraph::lookupNode(TypeVariableType *typeVar) {
  // Check whether we've already created a node for this type variable.
  auto &impl = typeVar->getImpl();
  if (auto nodePtr = impl.getGraphNode()) {
    assert(impl.getGraphIndex() < TypeVariables.size() && "Out-of-bounds index");
    assert(TypeVariables[impl.getGraphIndex()] == typeVar && 
           "Type variable mismatch");
    return { *nodePtr, impl.getGraphIndex() };
  }

  // Allocate the new node.
  auto nodePtr = new ConstraintGraphNode(typeVar);
  unsigned index = TypeVariables.size();
  impl.setGraphNode(nodePtr);
  impl.setGraphIndex(index);

  // Record this type variable.
  TypeVariables.push_back(typeVar);

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedTypeVariable(typeVar));

  // If this type variable is not the representative of its equivalence class,
  // add it to its representative's set of equivalences.
  auto typeVarRep = CS.getRepresentative(typeVar);
  if (typeVar != typeVarRep)
    mergeNodes(typeVar, typeVarRep);
  else if (auto fixed = CS.getFixedType(typeVarRep)) {
    // Bind the type variable.
    bindTypeVariable(typeVar, fixed);
  }

  return { *nodePtr, index };
}

ArrayRef<TypeVariableType *> ConstraintGraphNode::getEquivalenceClass() const{
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't request equivalence class from non-representative type var");
  return getEquivalenceClassUnsafe();
}

ArrayRef<TypeVariableType *>
ConstraintGraphNode::getEquivalenceClassUnsafe() const{
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  return EquivalenceClass;
}

#pragma mark Node mutation
void ConstraintGraphNode::addConstraint(Constraint *constraint) {
  assert(ConstraintIndex.count(constraint) == 0 && "Constraint re-insertion");
  ConstraintIndex[constraint] = Constraints.size();
  Constraints.push_back(constraint);
}

void ConstraintGraphNode::removeConstraint(Constraint *constraint) {
  auto pos = ConstraintIndex.find(constraint);
  assert(pos != ConstraintIndex.end());

  // Remove this constraint from the constraint mapping.
  auto index = pos->second;
  ConstraintIndex.erase(pos);
  assert(Constraints[index] == constraint && "Mismatched constraint");

  // If this is the last constraint, just pop it off the list and we're done.
  unsigned lastIndex = Constraints.size()-1;
  if (index == lastIndex) {
    Constraints.pop_back();
    return;
  }

  // This constraint is somewhere in the middle; swap it with the last
  // constraint, so we can remove the constraint from the vector in O(1)
  // time rather than O(n) time.
  auto lastConstraint = Constraints[lastIndex];
  Constraints[index] = lastConstraint;
  ConstraintIndex[lastConstraint] = index;
  Constraints.pop_back();
}

void ConstraintGraphNode::addToEquivalenceClass(
       ArrayRef<TypeVariableType *> typeVars) {
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't extend equivalence class of non-representative type var");
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  EquivalenceClass.append(typeVars.begin(), typeVars.end());
}

void ConstraintGraphNode::addFixedBinding(TypeVariableType *typeVar) {
  FixedBindings.push_back(typeVar);
}

void ConstraintGraphNode::removeFixedBinding(TypeVariableType *typeVar) {
  FixedBindings.pop_back();
}

#pragma mark Graph scope management
ConstraintGraphScope::ConstraintGraphScope(ConstraintGraph &CG)
  : CG(CG), ParentScope(CG.ActiveScope), NumChanges(CG.Changes.size())
{
  CG.ActiveScope = this;
}

ConstraintGraphScope::~ConstraintGraphScope() {
  // Pop changes off the stack until we hit the change could we had prior to
  // introducing this scope.
  assert(CG.Changes.size() >= NumChanges && "Scope stack corrupted");
  while (CG.Changes.size() > NumChanges) {
    CG.Changes.back().undo(CG);
    CG.Changes.pop_back();
  }

  // The active scope is now the parent scope.
  CG.ActiveScope = ParentScope;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedTypeVariable(TypeVariableType *typeVar) {
  Change result;
  result.Kind = ChangeKind::AddedTypeVariable;
  result.TypeVar = typeVar;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::AddedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::removedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::RemovedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::extendedEquivalenceClass(TypeVariableType *typeVar,
                                                  unsigned prevSize) {
  Change result;
  result.Kind = ChangeKind::ExtendedEquivalenceClass;
  result.EquivClass.TypeVar = typeVar;
  result.EquivClass.PrevSize = prevSize;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::boundTypeVariable(TypeVariableType *typeVar,
                                           Type fixed) {
  Change result;
  result.Kind = ChangeKind::BoundTypeVariable;
  result.Binding.TypeVar = typeVar;
  result.Binding.FixedType = fixed.getPointer();
  return result;
}

void ConstraintGraph::Change::undo(ConstraintGraph &cg) {
  /// Temporarily change the active scope to null, so we don't record
  /// any changes made while performing the undo operation.
  llvm::SaveAndRestore<ConstraintGraphScope *> prevActiveScope(cg.ActiveScope,
                                                               nullptr);

  switch (Kind) {
  case ChangeKind::AddedTypeVariable:
    cg.removeNode(TypeVar);
    break;

  case ChangeKind::AddedConstraint:
    cg.removeConstraint(TheConstraint);
    break;

  case ChangeKind::RemovedConstraint:
    cg.addConstraint(TheConstraint);
    break;

  case ChangeKind::ExtendedEquivalenceClass: {
    auto &node = cg[EquivClass.TypeVar];
    node.EquivalenceClass.erase(
      node.EquivalenceClass.begin() + EquivClass.PrevSize,
      node.EquivalenceClass.end());
    break;
   }

  case ChangeKind::BoundTypeVariable:
    cg.unbindTypeVariable(Binding.TypeVar, Binding.FixedType);
    break;
  }
}

#pragma mark Graph mutation

void ConstraintGraph::removeNode(TypeVariableType *typeVar) {
  // Remove this node.
  auto &impl = typeVar->getImpl();
  unsigned index = impl.getGraphIndex();
  delete impl.getGraphNode();
  impl.setGraphNode(nullptr);

  // Remove this type variable from the list.
  unsigned lastIndex = TypeVariables.size()-1;
  if (index < lastIndex)
    TypeVariables[index] = TypeVariables[lastIndex];
  TypeVariables.pop_back();
}

void ConstraintGraph::addConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Note the constraint within the node for that type variable.
    node.addConstraint(constraint);
  }

  // If the constraint doesn't reference any type variables, it's orphaned;
  // track it as such.
  if (referencedTypeVars.empty()) {
    OrphanedConstraints.push_back(constraint);
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedConstraint(constraint));
}

void ConstraintGraph::removeConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Remove the constraint.
    node.removeConstraint(constraint);
  }

  // If this is an orphaned constraint, remove it from the list.
  if (referencedTypeVars.empty()) {
    auto known = std::find(OrphanedConstraints.begin(),
                           OrphanedConstraints.end(),
                           constraint);
    assert(known != OrphanedConstraints.end() && "missing orphaned constraint");
    *known = OrphanedConstraints.back();
    OrphanedConstraints.pop_back();
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::removedConstraint(constraint));
}

void ConstraintGraph::mergeNodes(TypeVariableType *typeVar1, 
                                 TypeVariableType *typeVar2) {
  assert(CS.getRepresentative(typeVar1) == CS.getRepresentative(typeVar2) &&
         "type representatives don't match");
  
  // Retrieve the node for the representative that we're merging into.
  auto typeVarRep = CS.getRepresentative(typeVar1);
  auto &repNode = (*this)[typeVarRep];

  // Retrieve the node for the non-representative.
  assert((typeVar1 == typeVarRep || typeVar2 == typeVarRep) &&
         "neither type variable is the new representative?");
  auto typeVarNonRep = typeVar1 == typeVarRep? typeVar2 : typeVar1;

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::extendedEquivalenceClass(
                        typeVarRep,
                        repNode.getEquivalenceClass().size()));

  // Merge equivalence class from the non-representative type variable.
  auto &nonRepNode = (*this)[typeVarNonRep];
  repNode.addToEquivalenceClass(nonRepNode.getEquivalenceClassUnsafe());
}

void ConstraintGraph::bindTypeVariable(TypeVariableType *typeVar, Type fixed) {
  // If there are no type variables in the fixed type, there's nothing to do.
  if (!fixed->hasTypeVariable())
    return;

  SmallVector<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  fixed->getTypeVariables(typeVars);
  auto &node = (*this)[typeVar];
  for (auto otherTypeVar : typeVars) {
    if (knownTypeVars.insert(otherTypeVar).second) {
      if (typeVar == otherTypeVar) continue;

      (*this)[otherTypeVar].addFixedBinding(typeVar);
      node.addFixedBinding(otherTypeVar);
    }
  }

  // Record the change, if there are active scopes.
  // Note: If we ever use this to undo the actual variable binding,
  // we'll need to store the change along the early-exit path as well.
  if (ActiveScope)
    Changes.push_back(Change::boundTypeVariable(typeVar, fixed));
}

void ConstraintGraph::unbindTypeVariable(TypeVariableType *typeVar, Type fixed){
  // If there are no type variables in the fixed type, there's nothing to do.
  if (!fixed->hasTypeVariable())
    return;

  SmallVector<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  fixed->getTypeVariables(typeVars);
  auto &node = (*this)[typeVar];
  for (auto otherTypeVar : typeVars) {
    if (knownTypeVars.insert(otherTypeVar).second) {
      (*this)[otherTypeVar].removeFixedBinding(typeVar);
      node.removeFixedBinding(otherTypeVar);
    }
  }
}

llvm::TinyPtrVector<Constraint *> ConstraintGraph::gatherConstraints(
    TypeVariableType *typeVar, GatheringKind kind,
    llvm::function_ref<bool(Constraint *)> acceptConstraint) {
  llvm::TinyPtrVector<Constraint *> constraints;

  /// Add constraints for the given adjacent type variable.
  llvm::SmallPtrSet<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<Constraint *, 4> visitedConstraints;
  auto addAdjacentConstraints = [&](TypeVariableType *adjTypeVar) {
    auto adjTypeVarsToVisit =
        (*this)[CS.getRepresentative(adjTypeVar)].getEquivalenceClass();
    for (auto adjTypeVarEquiv : adjTypeVarsToVisit) {
      if (!typeVars.insert(adjTypeVarEquiv).second)
        continue;

      for (auto constraint : (*this)[adjTypeVarEquiv].getConstraints()) {
        if (!visitedConstraints.insert(constraint).second)
          continue;

        if (acceptConstraint(constraint))
          constraints.push_back(constraint);
      }
    }
  };

  auto &reprNode = (*this)[CS.getRepresentative(typeVar)];
  auto equivClass = reprNode.getEquivalenceClass();
  for (auto typeVar : equivClass) {
    auto &node = (*this)[typeVar];
    for (auto constraint : node.getConstraints()) {
      if (visitedConstraints.insert(constraint).second &&
          acceptConstraint(constraint))
        constraints.push_back(constraint);

      // If we want all mentions, visit type variables within each of our
      // constraints.
      if (kind == GatheringKind::AllMentions) {
        for (auto adjTypeVar : constraint->getTypeVariables()) {
          addAdjacentConstraints(adjTypeVar);
        }
      }
    }

    // For any type variable mentioned in a fixed binding, add adjacent
    // constraints.
    for (auto adjTypeVar : node.getFixedBindings()) {
      addAdjacentConstraints(adjTypeVar);
    }
  }

  return constraints;
}

#pragma mark Algorithms

namespace {
  /// A union-find connected components algorithm used to find the connected
  /// components within a constraint graph.
  class ConnectedComponents {
    ConstraintGraph &cg;
    ArrayRef<TypeVariableType *> typeVars;

    /// A mapping from each type variable to its representative in a union-find
    /// data structure, including entries where the type variable is its own
    /// representative.
    mutable llvm::SmallDenseMap<TypeVariableType *, TypeVariableType *>
        representatives;

    /// Describes one component within the directed graph of one-way
    /// constraints.
    struct RawOneWayComponent {
      /// All of the type variables in this one-way component.
      TinyPtrVector<TypeVariableType *> typeVars;

      /// The (uniqued) set of type variable representatives to which this
      /// component has an outgoing edge.
      TinyPtrVector<TypeVariableType *> outAdjacencies;

      /// The (uniqued) set of type variable representatives from which this
      /// component has an incoming edge.
      TinyPtrVector<TypeVariableType *> inAdjacencies;
    };

    // Adjacency list representation of the directed graph of edges for
    // one-way constraints, using type variable representatives as the
    // nodes.
    llvm::SmallDenseMap<TypeVariableType *, RawOneWayComponent> oneWayDigraph;

  public:
    using Component = ConstraintGraph::Component;
    using OneWayComponent = ConstraintGraph::OneWayComponent;

    /// Compute connected components for the given set of type variables
    /// in the constraint graph.
    ConnectedComponents(ConstraintGraph &cg,
                        ArrayRef<TypeVariableType *> typeVars)
        : cg(cg), typeVars(typeVars)
    {
      auto oneWayConstraints = connectedComponents();

      // If there were no one-way constraints, we're done.
      if (oneWayConstraints.empty())
        return;

      // Build the directed one-way constraint graph.
      buildOneWayConstraintGraph(oneWayConstraints);

      // Finish collapsing the connected components by joining sets based
      // on the one-way constraints.
      for (auto constraint : oneWayConstraints) {
        unionSetsViaConstraint(constraint);
      }
    }

    /// Retrieve the set of components.
    SmallVector<Component, 1> getComponents() const {
      // Find those type variables whose components involve unbound type
      // variables; these are the only components and type variables we want to
      // report.
      SmallPtrSet<TypeVariableType *, 4> componentHasUnboundTypeVar;
      auto &cs = cg.getConstraintSystem();
      for (auto typeVar : typeVars) {
        // If this type variable has a fixed type, skip it.
        if (cs.getFixedType(typeVar))
          continue;

        componentHasUnboundTypeVar.insert(findRepresentative(typeVar));
      }

      // Assign each type variable and constraint to its appropriate component.
      SmallVector<Component, 1> components;
      llvm::SmallDenseMap<TypeVariableType *, unsigned> componentIdxMap;
      SmallPtrSet<Constraint *, 4> knownConstraints;
      for (auto typeVar : typeVars) {
        // Find the representative. If it's component doesn't have an unbound
        // type variable, there's nothing to do.
        auto rep = findRepresentative(typeVar);
        if (componentHasUnboundTypeVar.count(rep) == 0)
          continue;

        // Find the component index.
        auto knownComponentIdx = componentIdxMap.find(rep);
        if (knownComponentIdx == componentIdxMap.end()) {
          // We haven't allocated this component yet; do so now.
          knownComponentIdx = componentIdxMap.insert(
              {rep, componentIdxMap.size()}).first;
          components.push_back({ });
        }

        // Record this type variable as part of the component.
        unsigned componentIdx = knownComponentIdx->second;
        auto &component = components[componentIdx];
        component.typeVars.push_back(typeVar);

        // Record this type variable's constraints as part of the component,
        // uniquing as we go.
        for (auto constraint : cg[typeVar].getConstraints()) {
          if (knownConstraints.insert(constraint).second) {
            component.constraints.push_back(constraint);
          }
        }
      }

      if (!oneWayDigraph.empty()) {
        populateOneWayComponentDependencies(componentIdxMap, components);
      }

      return components;
    }

    /// Find the representative for the given type variable within the set
    /// of representatives in a union-find data structure.
    TypeVariableType *findRepresentative(TypeVariableType *typeVar) const {
      // If we don't have a record of this type variable, it is it's own
      // representative.
      auto known = representatives.find(typeVar);
      if (known == representatives.end())
        return typeVar;

      // Find the representative of the parent.
      auto parent = known->second;
      auto rep = findRepresentative(parent);

      // Path compression.
      if (rep != parent)
        representatives[typeVar] = rep;

      return rep;
    }

    /// Perform the union of two type variables in a union-find data structure
    /// used for connected components.
    ///
    /// \returns true if the two components were separate and have now been joined,
    /// \c false if they were already in the same set.
    bool unionSets(TypeVariableType *typeVar1, TypeVariableType *typeVar2) {
      auto rep1 = findRepresentative(typeVar1);
      auto rep2 = findRepresentative(typeVar2);
      if (rep1 == rep2)
        return false;

      // Reparent the type variable with the higher ID. The actual choice doesn't
      // matter, but this makes debugging easier.
      if (rep1->getID() < rep2->getID())
        representatives[rep2] = rep1;
      else
        representatives[rep1] = rep2;
      return true;
    }

    /// Perform a union amongst the type variables referenced by the given
    /// constraint.
    ///
    /// \returns \c true if any components were joined by this constraint.
    bool unionSetsViaConstraint(Constraint *constraint) {
      auto typeVars = constraint->getTypeVariables();
      if (typeVars.size() < 2)
        return false;

      // Merge the first type variable with all of the others.
      bool anyUnioned = false;
      auto typeVar = typeVars.front();
      for (auto otherTypeVar : typeVars.drop_front()) {
        if (unionSets(typeVar, otherTypeVar))
          anyUnioned = true;
      }

      return anyUnioned;
    }

  private:
    /// Perform the connected components algorithm, skipping one-way
    /// constraints.
    ///
    /// \returns the set of one-way constraints encountered.
    TinyPtrVector<Constraint *> connectedComponents() {
      TinyPtrVector<Constraint *> oneWayConstraints;
      SmallPtrSet<Constraint *, 8> visitedConstraints;
      for (auto typeVar : typeVars) {
        auto rep = typeVar->getImpl().getRepresentative(nullptr);
        for (auto equiv : cg[rep].getEquivalenceClass())
          unionSets(typeVar, equiv);

        auto &node = cg[typeVar];
        for (auto fixedAdj : node.getFixedBindings())
          unionSets(typeVar, fixedAdj);

        for (auto constraint : node.getConstraints()) {
          if (!visitedConstraints.insert(constraint).second)
            continue;

          // Record and skip one-way constraints.
          if (constraint->getKind() == ConstraintKind::OneWayBind ||
              constraint->getKind() == ConstraintKind::OneWayBindParam) {
            oneWayConstraints.push_back(constraint);
            continue;
          }

          unionSetsViaConstraint(constraint);
        }
      }

      return oneWayConstraints;
    }

    /// Insert the given type variable into the given vector if it isn't
    /// already present.
    static void insertIfUnique(TinyPtrVector<TypeVariableType *> &vector,
                               TypeVariableType *typeVar) {
      if (std::find(vector.begin(), vector.end(), typeVar) == vector.end())
        vector.push_back(typeVar);
    }

    /// Build the directed graph of one-way constraints among components.
    void buildOneWayConstraintGraph(ArrayRef<Constraint *> oneWayConstraints)  {
      /// Retrieve the set of representatives for the type variables that
      /// occur within the given type.
      auto getRepresentativesInType = [&](Type type) {
        TinyPtrVector<TypeVariableType *> results;

        SmallVector<TypeVariableType *, 2> typeVars;
        type->getTypeVariables(typeVars);
        for (auto typeVar : typeVars) {
          auto rep = findRepresentative(typeVar);
          insertIfUnique(results, rep);
        }

        return results;
      };

      // Add all of the one-way constraint edges to the digraph.
      for (auto constraint : oneWayConstraints) {
        auto lhsTypeReps =
            getRepresentativesInType(constraint->getFirstType());
        auto rhsTypeReps =
            getRepresentativesInType(constraint->getSecondType());

        // Add an edge from the type representatives on the right-hand side
        // of the one-way constraint to the type representatives on the
        // left-hand side, because the right-hand type variables need to
        // be solved before the left-hand type variables.
        for (auto lhsTypeRep : lhsTypeReps) {
          for (auto rhsTypeRep : rhsTypeReps) {
            insertIfUnique(oneWayDigraph[rhsTypeRep].outAdjacencies,lhsTypeRep);
            insertIfUnique(oneWayDigraph[lhsTypeRep].inAdjacencies,rhsTypeRep);
          }
        }
      }

      // Put the type variables in their corresponding components in the
      // one-way directed graph.
      for (auto typeVar : typeVars) {
        auto rep = findRepresentative(typeVar);
        auto knownComponent = oneWayDigraph.find(rep);
        if (knownComponent == oneWayDigraph.end())
          continue;

        knownComponent->second.typeVars.push_back(typeVar);
      }
    }

    /// Perform a depth-first search over all of the type variables, calling
    /// the \c postVisit hook for each type variable after it has been visited.
    void postorderDepthFirstSearch(
        llvm::function_ref<
          ArrayRef<TypeVariableType *>(TypeVariableType *)> getAdjacencies,
        llvm::function_ref<void(TypeVariableType *)> postVisit) const {
      SmallPtrSet<TypeVariableType *, 4> visited;
      for (auto typeVar : typeVars) {
        postorderDepthFirstSearchRec(typeVar, getAdjacencies, postVisit,
                                     visited);
      }
    }

    /// Perform a depth-first search to produce a from the given type variable,
    /// notifying the function object \c postVisit after each reachable
    /// type variable has been visited.
    static void postorderDepthFirstSearchRec(
        TypeVariableType *typeVar,
        llvm::function_ref<
          ArrayRef<TypeVariableType *>(TypeVariableType *)> getAdjacencies,
        llvm::function_ref<void(TypeVariableType *)> postVisit,
        SmallPtrSet<TypeVariableType *, 4> &visited) {
      if (!visited.insert(typeVar).second)
        return;

      for (auto adj : getAdjacencies(typeVar)) {
        postorderDepthFirstSearchRec(adj, getAdjacencies, postVisit, visited);
      }

      postVisit(typeVar);
    }

    /// Populate each of the \c components with one-way dependency information
    /// in the form that will be evaluated by the constraint solver.
    void populateOneWayComponentDependencies(
        llvm::SmallDenseMap<TypeVariableType *, unsigned int> &componentIdxMap,
        MutableArrayRef<Component> components) const {
      // Perform a postorder depth-first search through the one-way digraph
      // to establish the dependency ordering for the type variables in each
      // component.
      //
      // Each entry in the (outer) vector corresponds to a component in the
      // (final) connected components (after contracting one-way constraints),
      // while the inner vectors consist of the type variables that
      // are representatives of the subcomponents (before contracting one-way
      // constraints), recorded in dependency order, meaning that
      // a type variable comes after all of the type variables it depends on.
      std::vector<TinyPtrVector<TypeVariableType *>> dependencyOrders;
      dependencyOrders.resize(components.size());
      postorderDepthFirstSearch(
          [&](TypeVariableType *typeVar) -> ArrayRef<TypeVariableType *> {
            // Find the component index for the representative.
            auto rep = findRepresentative(typeVar);
            auto knownComponentIdx = componentIdxMap.find(rep);
            if (knownComponentIdx == componentIdxMap.end()) {
              // The representative ia in a component that has no unbound
              // type variables, so don't traverse any further.
              return { };
            }

            // Traverse the outgoing adjacencies for the subcomponent
            auto oneWayComponent = oneWayDigraph.find(typeVar);
            if (oneWayComponent == oneWayDigraph.end()) {
              return { };
            }

            return oneWayComponent->second.outAdjacencies;
          },
          [&](TypeVariableType *typeVar) {
            // Skip any type variables that aren't represented in the one-way
            // directed graph.
            auto oneWayComponent = oneWayDigraph.find(typeVar);
            if (oneWayComponent == oneWayDigraph.end())
              return;

            // Find the component index of the representative.
            auto rep = findRepresentative(typeVar);
            auto knownComponentIdx = componentIdxMap.find(rep);
            if (knownComponentIdx == componentIdxMap.end()) {
              // Nothing to record for this type variable; it's in a component
              // that has no unbound type variables.
              return;
            }

            dependencyOrders[knownComponentIdx->second].push_back(typeVar);
          });

      // For each component in the final connected components, establish the
      // set of one-way components.
      for (unsigned componentIdx : indices(components)) {
        auto &dependencyOrder = dependencyOrders[componentIdx];
        if (dependencyOrder.empty()) {
          // No one-way constraints in this component.
          continue;
        }

        // Form the one-way components, copying over the type variables and
        // computing each of the dependencies.
        auto &component = components[componentIdx];
        llvm::SmallDenseMap<TypeVariableType *, unsigned> subcomponentIdxs;
        for (auto typeVar : reversed(dependencyOrder)) {
          assert(subcomponentIdxs.count(typeVar) == 0);
          subcomponentIdxs[typeVar] = component.oneWayComponents.size();

          // Add a one-way subcomponent with the type variables from the
          // corresponding "raw" one-way subcomponent.
          component.oneWayComponents.push_back({ });
          auto rawOneWayComponent = oneWayDigraph.find(typeVar);
          if (rawOneWayComponent != oneWayDigraph.end()) {
            component.oneWayComponents.back().typeVars =
            rawOneWayComponent->second.typeVars;
          } else {
            component.oneWayComponents.back().typeVars.push_back(typeVar);
          }

          // Perform a depth-first search search from this type variable
          // through the one-way component digraph, following the edges
          // *backward* to find all of the subcomponents on which this
          // component depends.
          SmallPtrSet<TypeVariableType *, 4> visited;
          postorderDepthFirstSearchRec(
              typeVar,
              [&](TypeVariableType *typeVar) -> ArrayRef<TypeVariableType *> {
                // Traverse the outgoing adjacencies for the subcomponent
                auto oneWayComponent = oneWayDigraph.find(typeVar);
                if (oneWayComponent == oneWayDigraph.end()) {
                  return { };
                }

                return oneWayComponent->second.inAdjacencies;
              },
              [&](TypeVariableType *dependsOn) {
                // We don't depend on ourselves.
                if (dependsOn == typeVar)
                  return;

                // Record the subcomponent index of this type variable in the
                // subcomponent; we depend on it.
                auto subcomponentIdx = subcomponentIdxs.find(dependsOn);
                assert(subcomponentIdx != subcomponentIdxs.end());
                component.oneWayComponents.back().dependsOn.push_back(
                  subcomponentIdx->second);
              },
              visited);
        }
      }
    }
  };
}

SmallVector<ConstraintGraph::Component, 1>
ConstraintGraph::computeConnectedComponents(
           ArrayRef<TypeVariableType *> typeVars) {
  // Perform connected components via a union-find algorithm on all of the
  // constraints adjacent to these type variables.
  ConnectedComponents cc(*this, typeVars);

  return cc.getComponents();
}

/// For a given constraint kind, decide if we should attempt to eliminate its
/// edge in the graph.
static bool shouldContractEdge(ConstraintKind kind) {
  switch (kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Equal:
    return true;

  default:
    return false;
  }
}

bool ConstraintGraph::contractEdges() {
  SmallVector<Constraint *, 16> constraints;
  CS.findConstraints(constraints, [&](const Constraint &constraint) {
    // Track how many constraints did contraction algorithm iterated over.
    incrementConstraintsPerContractionCounter();
    return shouldContractEdge(constraint.getKind());
  });

  bool didContractEdges = false;
  for (auto *constraint : constraints) {
    auto kind = constraint->getKind();

    // Contract binding edges between type variables.
    assert(shouldContractEdge(kind));

    auto t1 = constraint->getFirstType()->getDesugaredType();
    auto t2 = constraint->getSecondType()->getDesugaredType();

    auto tyvar1 = t1->getAs<TypeVariableType>();
    auto tyvar2 = t2->getAs<TypeVariableType>();

    if (!(tyvar1 && tyvar2))
      continue;

    auto isParamBindingConstraint = kind == ConstraintKind::BindParam;

    // If the argument is allowed to bind to `inout`, in general,
    // it's invalid to contract the edge between argument and parameter,
    // but if we can prove that there are no possible bindings
    // which result in attempt to bind `inout` type to argument
    // type variable, we should go ahead and allow (temporary)
    // contraction, because that greatly helps with performance.
    // Such action is valid because argument type variable can
    // only get its bindings from related overload, which gives
    // us enough information to decided on l-valueness.
    if (isParamBindingConstraint && tyvar1->getImpl().canBindToInOut()) {
      bool isNotContractable = true;
      if (auto bindings = CS.getPotentialBindings(tyvar1)) {
        for (auto &binding : bindings.Bindings) {
          auto type = binding.BindingType;
          isNotContractable = type.findIf([&](Type nestedType) -> bool {
            if (auto tv = nestedType->getAs<TypeVariableType>()) {
              if (tv->getImpl().canBindToInOut())
                return true;
            }

            return nestedType->is<InOutType>();
          });

          // If there is at least one non-contractable binding, let's
          // not risk contracting this edge.
          if (isNotContractable)
            break;
        }
      }

      if (isNotContractable)
        continue;
    }

    auto rep1 = CS.getRepresentative(tyvar1);
    auto rep2 = CS.getRepresentative(tyvar2);

    if (((rep1->getImpl().canBindToLValue() ==
          rep2->getImpl().canBindToLValue()) ||
         // Allow l-value contractions when binding parameter types.
         isParamBindingConstraint)) {
      if (CS.TC.getLangOpts().DebugConstraintSolver) {
        auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
        if (CS.solverState)
          log.indent(CS.solverState->depth * 2);

        log << "Contracting constraint ";
        constraint->print(log, &CS.getASTContext().SourceMgr);
        log << "\n";
      }

      // Merge the edges and remove the constraint.
      removeEdge(constraint);
      if (rep1 != rep2)
        CS.mergeEquivalenceClasses(rep1, rep2, /*updateWorkList*/ false);
      didContractEdges = true;
    }
  }
  return didContractEdges;
}

void ConstraintGraph::removeEdge(Constraint *constraint) {
  bool isExistingConstraint = false;

  for (auto &active : CS.ActiveConstraints) {
    if (&active == constraint) {
      CS.ActiveConstraints.erase(constraint);
      isExistingConstraint = true;
      break;
    }
  }

  for (auto &inactive : CS.InactiveConstraints) {
    if (&inactive == constraint) {
      CS.InactiveConstraints.erase(constraint);
      isExistingConstraint = true;
      break;
    }
  }

  if (CS.solverState) {
    if (isExistingConstraint)
      CS.solverState->retireConstraint(constraint);
    else
      CS.solverState->removeGeneratedConstraint(constraint);
  }

  removeConstraint(constraint);
}

void ConstraintGraph::optimize() {
  // Merge equivalence classes until a fixed point is reached.
  while (contractEdges()) {}
}

void ConstraintGraph::incrementConstraintsPerContractionCounter() {
  SWIFT_FUNC_STAT;
  auto &context = CS.getASTContext();
  if (context.Stats)
    context.Stats->getFrontendCounters()
        .NumConstraintsConsideredForEdgeContraction++;
}

#pragma mark Debugging output

void ConstraintGraphNode::print(llvm::raw_ostream &out, unsigned indent) {
  out.indent(indent);
  TypeVar->print(out);
  out << ":\n";

  // Print constraints.
  if (!Constraints.empty()) {
    out.indent(indent + 2);
    out << "Constraints:\n";
    SmallVector<Constraint *, 4> sortedConstraints(Constraints.begin(),
                                                   Constraints.end());
    std::sort(sortedConstraints.begin(), sortedConstraints.end());
    for (auto constraint : sortedConstraints) {
      out.indent(indent + 4);
      constraint->print(out, &TypeVar->getASTContext().SourceMgr);
      out << "\n";
    }
  }

  // Print fixed bindings.
  if (!FixedBindings.empty()) {
    out.indent(indent + 2);
    out << "Fixed bindings: ";
    SmallVector<TypeVariableType *, 4> sortedFixedBindings(
        FixedBindings.begin(), FixedBindings.end());
    std::sort(sortedFixedBindings.begin(), sortedFixedBindings.end(),
              [&](TypeVariableType *typeVar1, TypeVariableType *typeVar2) {
                return typeVar1->getID() < typeVar2->getID();
              });

    interleave(sortedFixedBindings,
               [&](TypeVariableType *typeVar) {
                 out << "$T" << typeVar->getID();
               },
               [&]() {
                 out << ", ";
               });
    out << "\n";
  }

  // Print equivalence class.
  if (TypeVar->getImpl().getRepresentative(nullptr) == TypeVar &&
      EquivalenceClass.size() > 1) {
    out.indent(indent + 2);
    out << "Equivalence class:";
    for (unsigned i = 1, n = EquivalenceClass.size(); i != n; ++i) {
      out << ' ';
      EquivalenceClass[i]->print(out);
    }
    out << "\n";
  }
}

void ConstraintGraphNode::dump() {
  llvm::SaveAndRestore<bool>
    debug(TypeVar->getASTContext().LangOpts.DebugConstraintSolver, true);
  print(llvm::dbgs(), 0);
}

void ConstraintGraph::print(llvm::raw_ostream &out) {
  for (auto typeVar : TypeVariables) {
    (*this)[typeVar].print(out, 2);
    out << "\n";
  }
}

void ConstraintGraph::dump() {
  llvm::SaveAndRestore<bool>
    debug(CS.getASTContext().LangOpts.DebugConstraintSolver, true);
  print(llvm::dbgs());
}

void ConstraintGraph::printConnectedComponents(llvm::raw_ostream &out) {
  auto components = computeConnectedComponents(TypeVariables);
  for (unsigned componentIdx : indices(components)) {
    out.indent(2);
    out << componentIdx << ": ";
    auto &component = components[componentIdx];
    SWIFT_DEFER {
      out << '\n';
    };

    // Print all of the type variables in this connected component.
    interleave(component.typeVars,
               [&](TypeVariableType *typeVar) {
                 typeVar->print(out);
               },
               [&] {
                 out << ' ';
               });

    if (component.oneWayComponents.empty())
      continue;

    // Print all of the one-way components.
    out << ", one way components = ";
    interleave(component.oneWayComponents,
        [&](const ConstraintGraph::OneWayComponent &component) {
          out << '{';
          interleave(component.typeVars,
                     [&](TypeVariableType *typeVar) {
                       typeVar->print(out);
                     },
                     [&] {
                       out << ' ';
                     });
          if (!component.dependsOn.empty()) {
            out << " depends on ";
            interleave(
                component.dependsOn,
                [&](unsigned index) { out << index; },
                [&] { out << ", "; }
              );
          }
          out << '}';
        },
        [&] {
          out << ' ';
        });
  }
}

void ConstraintGraph::dumpConnectedComponents() {
  printConnectedComponents(llvm::dbgs());
}

#pragma mark Verification of graph invariants

/// Require that the given condition evaluate true.
///
/// If the condition is not true, complain about the problem and abort.
///
/// \param condition The actual Boolean condition.
///
/// \param complaint A string that describes the problem.
///
/// \param cg The constraint graph that failed verification.
///
/// \param node If non-null, the graph node that failed verification.
///
/// \param extraContext If provided, a function that will be called to
/// provide extra, contextual information about the failure.
static void _require(bool condition, const Twine &complaint,
                     ConstraintGraph &cg,
                     ConstraintGraphNode *node,
                     const std::function<void()> &extraContext = nullptr) {
  if (condition)
    return;

  // Complain
  llvm::dbgs() << "Constraint graph verification failed: " << complaint << '\n';
  if (extraContext)
    extraContext();

  // Print the graph.
  // FIXME: Highlight the offending node/constraint/etc.
  cg.print(llvm::dbgs());

  abort();
}

/// Print a type variable value.
static void printValue(llvm::raw_ostream &os, TypeVariableType *typeVar) {
  typeVar->print(os);
}

/// Print a constraint value.
static void printValue(llvm::raw_ostream &os, Constraint *constraint) {
  constraint->print(os, nullptr);
}

/// Print an unsigned value.
static void printValue(llvm::raw_ostream &os, unsigned value) {
  os << value;
}

void ConstraintGraphNode::verify(ConstraintGraph &cg) {
#define require(condition, complaint) _require(condition, complaint, cg, this)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, cg, this, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, cg, this, [&] {         \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the constraint map/vector haven't gotten out of sync.
  requireSameValue(Constraints.size(), ConstraintIndex.size(),
                   "constraint vector and map have different sizes");
  for (auto info : ConstraintIndex) {
    require(info.second < Constraints.size(), "constraint index out-of-range");
    requireSameValue(info.first, Constraints[info.second],
                     "constraint map provides wrong index into vector");
  }
#undef requireSameValue
#undef requireWithContext
#undef require
}

void ConstraintGraph::verify() {
#define require(condition, complaint) \
  _require(condition, complaint, *this, nullptr)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, *this, nullptr, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, *this, nullptr, [&] {   \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the type variables are either representatives or represented
  // within their representative's equivalence class.
  // FIXME: Also check to make sure the equivalence classes aren't too large?
  for (auto typeVar : TypeVariables) {
    auto typeVarRep = CS.getRepresentative(typeVar);
    auto &repNode = (*this)[typeVarRep];
    if (typeVar != typeVarRep) {
      // This type variable should be in the equivalence class of its
      // representative.
      require(std::find(repNode.getEquivalenceClass().begin(),
                        repNode.getEquivalenceClass().end(),
                        typeVar) != repNode.getEquivalenceClass().end(),
              "type variable not present in its representative's equiv class");
    } else {
      // Each of the type variables in the same equivalence class as this type
      // should have this type variable as their representative.
      for (auto equiv : repNode.getEquivalenceClass()) {
        requireSameValue(
          typeVar, equiv->getImpl().getRepresentative(nullptr),
          "representative and an equivalent type variable's representative");
      }
    }
  }

  // Verify that our type variable map/vector are in sync.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    requireSameValue(impl.getGraphIndex(), i, "wrong graph node index");
    require(impl.getGraphNode(), "null graph node");
  }

  // Verify consistency of all of the nodes in the graph.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    impl.getGraphNode()->verify(*this);
  }

  // Collect all of the constraints known to the constraint graph.
  llvm::SmallPtrSet<Constraint *, 4> knownConstraints;
  for (auto typeVar : getTypeVariables()) {
    for (auto constraint : (*this)[typeVar].getConstraints())
      knownConstraints.insert(constraint);
  }

  // Verify that all of the constraints in the constraint system
  // are accounted for.
  for (auto &constraint : CS.getConstraints()) {
    // Check whether the constraint graph knows about this constraint.
    auto referencedTypeVars = constraint.getTypeVariables();
    requireWithContext((knownConstraints.count(&constraint) ||
                        referencedTypeVars.empty()),
                       "constraint graph doesn't know about constraint",
                       [&] {
                         llvm::dbgs() << "constraint = ";
                         printValue(llvm::dbgs(), &constraint);
                         llvm::dbgs() << "\n";
                       });

    // Make sure each of the type variables referenced knows about this
    // constraint.
    for (auto typeVar : referencedTypeVars) {
      auto nodePtr = typeVar->getImpl().getGraphNode();
      requireWithContext(nodePtr,
                         "type variable in constraint not known",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });

      auto &node = *nodePtr;
      auto constraintPos = node.ConstraintIndex.find(&constraint);
      requireWithContext(constraintPos != node.ConstraintIndex.end(),
                         "type variable doesn't know about constraint",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });
    }
  }

#undef requireSameValue
#undef requireWithContext
#undef require
}


