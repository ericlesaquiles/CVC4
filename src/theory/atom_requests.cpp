/*********************                                                        */
/*! \file atom_requests.cpp
 ** \verbatim
 ** Original author: Dejan Jovanovic
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2013  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief [[ Add one-line brief description here ]]
 **
 ** [[ Add lengthier description here ]]
 ** \todo document this file
 **/

#include "theory/atom_requests.h"

using namespace CVC4;

AtomRequests::AtomRequests(context::Context* context)
: d_allRequests(context)
, d_requests(context)
, d_triggerToRequestMap(context)
{}

AtomRequests::element_index AtomRequests::getList(TNode trigger) const {
  trigger_to_list_map::const_iterator find = d_triggerToRequestMap.find(trigger);
  if (find == d_triggerToRequestMap.end()) {
    return null_index;
  } else {
    return (*find).second;
  }
}

bool AtomRequests::isTrigger(TNode atom) const {
  return getList(atom) != null_index;
}

AtomRequests::atom_iterator AtomRequests::getAtomIterator(TNode trigger) const {
  return atom_iterator(*this, getList(trigger));
}

void AtomRequests::add(TNode triggerAtom, TNode atomToSend, theory::TheoryId toTheory) {

  Debug("theory::atoms") << "AtomRequests::add(" << triggerAtom << ", " << atomToSend << ", " << toTheory << ")" << std::endl;

  Request request(atomToSend, toTheory);

  if (d_allRequests.find(request) != d_allRequests.end()) {
    // Have it already
    Debug("theory::atoms") << "AtomRequests::add(" << triggerAtom << ", " << atomToSend << ", " << toTheory << "): already there" << std::endl;
    return;
  }
  Debug("theory::atoms") << "AtomRequests::add(" << triggerAtom << ", " << atomToSend << ", " << toTheory << "): adding" << std::endl;

  /// Mark the new request
  d_allRequests.insert(request);

  // Index of the new request in the list of trigger
  element_index index = d_requests.size();
  element_index previous = getList(triggerAtom);
  d_requests.push_back(Element(request, previous));
  d_triggerToRequestMap[triggerAtom] = index;
}

bool AtomRequests::atom_iterator::done() const {
  return index == null_index;
}

void AtomRequests::atom_iterator::next() {
  index = requests.d_requests[index].previous;
}

const AtomRequests::Request& AtomRequests::atom_iterator::get() const {
  return requests.d_requests[index].request;
}

