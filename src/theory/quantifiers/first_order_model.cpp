/*********************                                                        */
/*! \file first_order_model.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds, Tim King, Morgan Deters
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2018 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of model engine model class
 **/

#include "theory/quantifiers/first_order_model.h"
#include "options/base_options.h"
#include "options/quantifiers_options.h"
#include "theory/quantifiers/fmf/ambqi_builder.h"
#include "theory/quantifiers/fmf/bounded_integers.h"
#include "theory/quantifiers/fmf/full_model_check.h"
#include "theory/quantifiers/fmf/model_engine.h"
#include "theory/quantifiers/quantifiers_attributes.h"
#include "theory/quantifiers/term_database.h"
#include "theory/quantifiers/term_enumeration.h"
#include "theory/quantifiers/term_util.h"

using namespace std;
using namespace CVC4::kind;
using namespace CVC4::context;
using namespace CVC4::theory::quantifiers::fmcheck;

namespace CVC4 {
namespace theory {
namespace quantifiers {

struct sortQuantifierRelevance {
  FirstOrderModel * d_fm;
  bool operator() (Node i, Node j) {
    int wi = d_fm->getRelevanceValue( i );
    int wj = d_fm->getRelevanceValue( j );
    if( wi==wj ){
      return i<j;
    }else{
      return wi<wj;
    }
  }
};

RepSetIterator::RsiEnumType QRepBoundExt::setBound(Node owner,
                                                   unsigned i,
                                                   std::vector<Node>& elements)
{
  // builtin: check if it is bound by bounded integer module
  if (owner.getKind() == FORALL && d_qe->getBoundedIntegers())
  {
    if (d_qe->getBoundedIntegers()->isBoundVar(owner, owner[0][i]))
    {
      unsigned bvt =
          d_qe->getBoundedIntegers()->getBoundVarType(owner, owner[0][i]);
      if (bvt != BoundedIntegers::BOUND_FINITE)
      {
        d_bound_int[i] = true;
        return RepSetIterator::ENUM_BOUND_INT;
      }
      else
      {
        // indicates the variable is finitely bound due to
        // the (small) cardinality of its type,
        // will treat in default way
      }
    }
  }
  return RepSetIterator::ENUM_INVALID;
}

bool QRepBoundExt::resetIndex(RepSetIterator* rsi,
                              Node owner,
                              unsigned i,
                              bool initial,
                              std::vector<Node>& elements)
{
  if (d_bound_int.find(i) != d_bound_int.end())
  {
    Assert(owner.getKind() == FORALL);
    Assert(d_qe->getBoundedIntegers() != nullptr);
    if (!d_qe->getBoundedIntegers()->getBoundElements(
            rsi, initial, owner, owner[0][i], elements))
    {
      return false;
    }
  }
  return true;
}

bool QRepBoundExt::initializeRepresentativesForType(TypeNode tn)
{
  return d_qe->getModel()->initializeRepresentativesForType(tn);
}

bool QRepBoundExt::getVariableOrder(Node owner, std::vector<unsigned>& varOrder)
{
  // must set a variable index order based on bounded integers
  if (owner.getKind() == FORALL && d_qe->getBoundedIntegers())
  {
    Trace("bound-int-rsi") << "Calculating variable order..." << std::endl;
    for (unsigned i = 0; i < d_qe->getBoundedIntegers()->getNumBoundVars(owner);
         i++)
    {
      Node v = d_qe->getBoundedIntegers()->getBoundVar(owner, i);
      Trace("bound-int-rsi") << "  bound var #" << i << " is " << v
                             << std::endl;
      varOrder.push_back(d_qe->getTermUtil()->getVariableNum(owner, v));
    }
    for (unsigned i = 0; i < owner[0].getNumChildren(); i++)
    {
      if (!d_qe->getBoundedIntegers()->isBoundVar(owner, owner[0][i]))
      {
        varOrder.push_back(i);
      }
    }
    return true;
  }
  return false;
}

FirstOrderModel::FirstOrderModel(QuantifiersEngine * qe, context::Context* c, std::string name ) :
TheoryModel( c, name, true ),
d_qe( qe ), d_forall_asserts( c ){
  d_rlv_count = 0;
}

void FirstOrderModel::assertQuantifier( Node n ){
  if( n.getKind()==FORALL ){
    d_forall_asserts.push_back( n );
  }else if( n.getKind()==NOT ){
    Assert( n[0].getKind()==FORALL );
  }
}

unsigned FirstOrderModel::getNumAssertedQuantifiers() { 
  return d_forall_asserts.size(); 
}

Node FirstOrderModel::getAssertedQuantifier( unsigned i, bool ordered ) { 
  if( !ordered ){
    return d_forall_asserts[i]; 
  }else{
    Assert( d_forall_rlv_assert.size()==d_forall_asserts.size() );
    return d_forall_rlv_assert[i];
  }
}

void FirstOrderModel::initialize() {
  processInitialize( true );
  //this is called after representatives have been chosen and the equality engine has been built
  //for each quantifier, collect all operators we care about
  for( unsigned i=0; i<getNumAssertedQuantifiers(); i++ ){
    Node f = getAssertedQuantifier( i );
    if( d_quant_var_id.find( f )==d_quant_var_id.end() ){
      for(unsigned j=0; j<f[0].getNumChildren(); j++){
        d_quant_var_id[f][f[0][j]] = j;
      }
    }
    processInitializeQuantifier( f );
    //initialize relevant models within bodies of all quantifiers
    std::map< Node, bool > visited;
    initializeModelForTerm( f[1], visited );
  }
  processInitialize( false );
}

void FirstOrderModel::initializeModelForTerm( Node n, std::map< Node, bool >& visited ){
  if( visited.find( n )==visited.end() ){
    visited[n] = true;
    processInitializeModelForTerm( n );
    for( int i=0; i<(int)n.getNumChildren(); i++ ){
      initializeModelForTerm( n[i], visited );
    }
  }
}

Node FirstOrderModel::getSomeDomainElement(TypeNode tn){
  //check if there is even any domain elements at all
  if (!d_rep_set.hasType(tn) || d_rep_set.d_type_reps[tn].size() == 0)
  {
    Trace("fm-debug") << "Must create domain element for " << tn << "..."
                      << std::endl;
    Node mbt = getModelBasisTerm(tn);
    Trace("fm-debug") << "Add to representative set..." << std::endl;
    d_rep_set.add(tn, mbt);
  }
  return d_rep_set.d_type_reps[tn][0];
}

bool FirstOrderModel::initializeRepresentativesForType(TypeNode tn)
{
  if (tn.isSort())
  {
    // must ensure uninterpreted type is non-empty.
    if (!d_rep_set.hasType(tn))
    {
      // terms in rep_set are now constants which mapped to terms through
      // TheoryModel. Thus, should introduce a constant and a term.
      // For now, we just add an arbitrary term.
      Node var = d_qe->getModel()->getSomeDomainElement(tn);
      Trace("mkVar") << "RepSetIterator:: Make variable " << var << " : " << tn
                     << std::endl;
      d_rep_set.add(tn, var);
    }
    return true;
  }
  else
  {
    // can we complete it?
    if (d_qe->getTermEnumeration()->mayComplete(tn))
    {
      Trace("fm-debug") << "  do complete, since cardinality is small ("
                        << tn.getCardinality() << ")..." << std::endl;
      d_rep_set.complete(tn);
      // must have succeeded
      Assert(d_rep_set.hasType(tn));
      return true;
    }
    Trace("fm-debug") << "  variable cannot be bounded." << std::endl;
    return false;
  }
}

/** needs check */
bool FirstOrderModel::checkNeeded() {
  return d_forall_asserts.size()>0;
}

void FirstOrderModel::reset_round() {
  d_quant_active.clear();
  
  //order the quantified formulas
  d_forall_rlv_assert.clear();
  if( !d_forall_rlv_vec.empty() ){
    Trace("fm-relevant") << "Build sorted relevant list..." << std::endl;
    Trace("fm-relevant-debug") << "Mark asserted quantified formulas..." << std::endl;
    std::map< Node, bool > qassert;
    for( unsigned i=0; i<d_forall_asserts.size(); i++ ){
      qassert[d_forall_asserts[i]] = true;
    }
    Trace("fm-relevant-debug") << "Sort the relevant quantified formulas..." << std::endl;
    sortQuantifierRelevance sqr;
    sqr.d_fm = this;
    std::sort( d_forall_rlv_vec.begin(), d_forall_rlv_vec.end(), sqr );
    Trace("fm-relevant-debug") << "Add relevant asserted formulas..." << std::endl;
    for( int i=(int)(d_forall_rlv_vec.size()-1); i>=0; i-- ){
      Node q = d_forall_rlv_vec[i];
      if( qassert.find( q )!=qassert.end() ){
        Trace("fm-relevant") << "   " << d_forall_rlv[q] << " : " << q << std::endl;
        d_forall_rlv_assert.push_back( q );
      }
    }
    Trace("fm-relevant-debug") << "Add remaining asserted formulas..." << std::endl;
    for( unsigned i=0; i<d_forall_asserts.size(); i++ ){
      Node q = d_forall_asserts[i];
      if( std::find( d_forall_rlv_assert.begin(), d_forall_rlv_assert.end(), q )==d_forall_rlv_assert.end() ){
        d_forall_rlv_assert.push_back( q );
      }else{
        Trace("fm-relevant-debug") << "...already included " << q << std::endl;
      }
    }
    Trace("fm-relevant-debug") << "Sizes : " << d_forall_rlv_assert.size() << " " << d_forall_asserts.size() << std::endl;
    Assert( d_forall_rlv_assert.size()==d_forall_asserts.size() );
  }else{
    for( unsigned i=0; i<d_forall_asserts.size(); i++ ){
      d_forall_rlv_assert.push_back( d_forall_asserts[i] );
    }
  }
}

void FirstOrderModel::markRelevant( Node q ) {
  if( q!=d_last_forall_rlv ){
    Trace("fm-relevant") << "Mark relevant : " << q << std::endl;
    if( std::find( d_forall_rlv_vec.begin(), d_forall_rlv_vec.end(), q )==d_forall_rlv_vec.end() ){
      d_forall_rlv_vec.push_back( q );
    }
    d_forall_rlv[ q ] = d_rlv_count;
    d_rlv_count++;
    d_last_forall_rlv = q;
  }
}

int FirstOrderModel::getRelevanceValue( Node q ) {
  std::map< Node, unsigned >::iterator it = d_forall_rlv.find( q );
  if( it==d_forall_rlv.end() ){
    return -1;
  }else{
    return it->second;
  }
}

void FirstOrderModel::setQuantifierActive( TNode q, bool active ) {
  d_quant_active[q] = active;
}

bool FirstOrderModel::isQuantifierActive( TNode q ) {
  std::map< TNode, bool >::iterator it = d_quant_active.find( q );
  if( it==d_quant_active.end() ){
    return true;
  }else{
    return it->second;
  }
}

bool FirstOrderModel::isQuantifierAsserted( TNode q ) {
  Assert( d_forall_rlv_assert.size()==d_forall_asserts.size() );
  return std::find( d_forall_rlv_assert.begin(), d_forall_rlv_assert.end(), q )!=d_forall_rlv_assert.end();
}

Node FirstOrderModel::getModelBasisTerm(TypeNode tn)
{
  if (d_model_basis_term.find(tn) == d_model_basis_term.end())
  {
    Node mbt;
    if (tn.isClosedEnumerable())
    {
      mbt = d_qe->getTermEnumeration()->getEnumerateTerm(tn, 0);
    }
    else
    {
      if (options::fmfFreshDistConst())
      {
        mbt = d_qe->getTermDatabase()->getOrMakeTypeFreshVariable(tn);
      }
      else
      {
        mbt = d_qe->getTermDatabase()->getOrMakeTypeGroundTerm(tn);
      }
    }
    ModelBasisAttribute mba;
    mbt.setAttribute(mba, true);
    d_model_basis_term[tn] = mbt;
    Trace("model-basis-term") << "Choose " << mbt << " as model basis term for "
                              << tn << std::endl;
  }
  return d_model_basis_term[tn];
}

bool FirstOrderModel::isModelBasisTerm(Node n)
{
  return n == getModelBasisTerm(n.getType());
}

Node FirstOrderModel::getModelBasisOpTerm(Node op)
{
  if (d_model_basis_op_term.find(op) == d_model_basis_op_term.end())
  {
    TypeNode t = op.getType();
    std::vector<Node> children;
    children.push_back(op);
    for (int i = 0; i < (int)(t.getNumChildren() - 1); i++)
    {
      children.push_back(getModelBasisTerm(t[i]));
    }
    if (children.size() == 1)
    {
      d_model_basis_op_term[op] = op;
    }
    else
    {
      d_model_basis_op_term[op] =
          NodeManager::currentNM()->mkNode(APPLY_UF, children);
    }
  }
  return d_model_basis_op_term[op];
}

Node FirstOrderModel::getModelBasis(Node q, Node n)
{
  // make model basis
  if (d_model_basis_terms.find(q) == d_model_basis_terms.end())
  {
    for (unsigned j = 0; j < q[0].getNumChildren(); j++)
    {
      d_model_basis_terms[q].push_back(getModelBasisTerm(q[0][j].getType()));
    }
  }
  Node gn = d_qe->getTermUtil()->substituteInstConstants(
      n, q, d_model_basis_terms[q]);
  return gn;
}

void FirstOrderModel::computeModelBasisArgAttribute(Node n)
{
  if (!n.hasAttribute(ModelBasisArgAttribute()))
  {
    // ensure that the model basis terms have been defined
    if (n.getKind() == APPLY_UF)
    {
      getModelBasisOpTerm(n.getOperator());
    }
    uint64_t val = 0;
    // determine if it has model basis attribute
    for (unsigned j = 0, nchild = n.getNumChildren(); j < nchild; j++)
    {
      if (n[j].getAttribute(ModelBasisAttribute()))
      {
        val++;
      }
    }
    ModelBasisArgAttribute mbaa;
    n.setAttribute(mbaa, val);
  }
}

unsigned FirstOrderModel::getModelBasisArg(Node n)
{
  computeModelBasisArgAttribute(n);
  return n.getAttribute(ModelBasisArgAttribute());
}

Node FirstOrderModelIG::UfModelTreeGenerator::getIntersection(TheoryModel* m,
                                                              Node n1,
                                                              Node n2,
                                                              bool& isGround)
{
  isGround = true;
  std::vector<Node> children;
  children.push_back(n1.getOperator());
  for (unsigned i = 0, size = n1.getNumChildren(); i < size; i++)
  {
    if (n1[i] == n2[i])
    {
      if (n1[i].getAttribute(ModelBasisAttribute()))
      {
        isGround = false;
      }
      children.push_back(n1[i]);
    }
    else if (n1[i].getAttribute(ModelBasisAttribute()))
    {
      children.push_back(n2[i]);
    }
    else if (n2[i].getAttribute(ModelBasisAttribute()))
    {
      children.push_back(n1[i]);
    }
    else if (m->areEqual(n1[i], n2[i]))
    {
      children.push_back(n1[i]);
    }
    else
    {
      return Node::null();
    }
  }
  return NodeManager::currentNM()->mkNode(APPLY_UF, children);
}

void FirstOrderModelIG::UfModelTreeGenerator::setValue(
    TheoryModel* m, Node n, Node v, bool ground, bool isReq)
{
  Assert(!n.isNull());
  Assert(!v.isNull());
  d_set_values[isReq ? 1 : 0][ground ? 1 : 0][n] = v;
  if (!ground)
  {
    for (unsigned i = 0, defSize = d_defaults.size(); i < defSize; i++)
    {
      // for correctness, to allow variable order-independent function
      // interpretations, we must ensure that the intersection of all default
      // terms is also defined.
      // for example, if we have that f( e, a ) = ..., and f( b, e ) = ...,
      // then we must define f( b, a ).
      bool isGround;
      Node ni = getIntersection(m, n, d_defaults[i], isGround);
      if (!ni.isNull())
      {
        // if the intersection exists, and is not already defined
        if (d_set_values[0][isGround ? 1 : 0].find(ni)
                == d_set_values[0][isGround ? 1 : 0].end()
            && d_set_values[1][isGround ? 1 : 0].find(ni)
                   == d_set_values[1][isGround ? 1 : 0].end())
        {
          // use the current value
          setValue(m, ni, v, isGround, false);
        }
      }
    }
    d_defaults.push_back(n);
  }
  if (isReq
      && d_set_values[0][ground ? 1 : 0].find(n)
             != d_set_values[0][ground ? 1 : 0].end())
  {
    d_set_values[0][ground ? 1 : 0].erase(n);
  }
}

void FirstOrderModelIG::UfModelTreeGenerator::makeModel(TheoryModel* m,
                                                        uf::UfModelTree& tree)
{
  for (int j = 0; j < 2; j++)
  {
    for (int k = 0; k < 2; k++)
    {
      for (std::map<Node, Node>::iterator it = d_set_values[j][k].begin();
           it != d_set_values[j][k].end();
           ++it)
      {
        tree.setValue(m, it->first, it->second, k == 1);
      }
    }
  }
  if (!d_default_value.isNull())
  {
    tree.setDefaultValue(m, d_default_value);
  }
  tree.simplify();
}

void FirstOrderModelIG::UfModelTreeGenerator::clear()
{
  d_default_value = Node::null();
  for (int j = 0; j < 2; j++)
  {
    for (int k = 0; k < 2; k++)
    {
      d_set_values[j][k].clear();
    }
  }
  d_defaults.clear();
}

FirstOrderModelIG::FirstOrderModelIG(QuantifiersEngine * qe, context::Context* c, std::string name) :
FirstOrderModel(qe, c,name) {

}

void FirstOrderModelIG::processInitialize( bool ispre ){
  if( ispre ){
    //rebuild models
    d_uf_model_tree.clear();
    d_uf_model_gen.clear();
  }
}

void FirstOrderModelIG::processInitializeModelForTerm( Node n ){
  if( n.getKind()==APPLY_UF ){
    Node op = n.getOperator();
    if( d_uf_model_tree.find( op )==d_uf_model_tree.end() ){
      TypeNode tn = op.getType();
      tn = tn[ (int)tn.getNumChildren()-1 ];
      //only generate models for predicates and functions with uninterpreted range types
      //if( tn==NodeManager::currentNM()->booleanType() || tn.isSort() ){
        d_uf_model_tree[ op ] = uf::UfModelTree( op );
        d_uf_model_gen[ op ].clear();
      //}
    }
  }
  /*
  if( n.getType().isArray() ){
    while( n.getKind()==STORE ){
      n = n[0];
    }
    Node nn = getRepresentative( n );
    if( d_array_model.find( nn )==d_array_model.end() ){
      d_array_model[nn] = arrays::ArrayModel( nn, this );
    }
  }
  */
}

//for evaluation of quantifier bodies

void FirstOrderModelIG::resetEvaluate(){
  d_eval_uf_use_default.clear();
  d_eval_uf_model.clear();
  d_eval_term_index_order.clear();
}

//if evaluate( n ) = eVal,
// let n' = ri * n be the formula n instantiated with the current values in r_iter
// if eVal = 1, then n' is true, if eVal = -1, then n' is false,
// if eVal = 0, then n' cannot be proven to be equal to phaseReq
// if eVal is not 0, then
//   each n{ri->d_index[0]/x_0...ri->d_index[depIndex]/x_depIndex, */x_(depIndex+1) ... */x_n } is equivalent in the current model
int FirstOrderModelIG::evaluate( Node n, int& depIndex, RepSetIterator* ri ){
  Debug("fmf-eval-debug2") << "Evaluate " << n << std::endl;
  //Notice() << "Eval " << n << std::endl;
  if( n.getKind()==NOT ){
    int val = evaluate( n[0], depIndex, ri );
    return val==1 ? -1 : ( val==-1 ? 1 : 0 );
  }else if( n.getKind()==OR || n.getKind()==AND ){
    int baseVal = n.getKind()==AND ? 1 : -1;
    int eVal = baseVal;
    int posDepIndex = ri->getNumTerms();
    int negDepIndex = -1;
    for( int i=0; i<(int)n.getNumChildren(); i++ ){
      //evaluate subterm
      int childDepIndex;
      Node nn = n[i];
      int eValT = evaluate( nn, childDepIndex, ri );
      if( eValT==baseVal ){
        if( eVal==baseVal ){
          if( childDepIndex>negDepIndex ){
            negDepIndex = childDepIndex;
          }
        }
      }else if( eValT==-baseVal ){
        eVal = -baseVal;
        if( childDepIndex<posDepIndex ){
          posDepIndex = childDepIndex;
          if( posDepIndex==-1 ){
            break;
          }
        }
      }else if( eValT==0 ){
        if( eVal==baseVal ){
          eVal = 0;
        }
      }
    }
    if( eVal!=0 ){
      depIndex = eVal==-baseVal ? posDepIndex : negDepIndex;
      return eVal;
    }else{
      return 0;
    }
  }else if( n.getKind()==EQUAL && n[0].getType().isBoolean() ){
    int depIndex1;
    int eVal = evaluate( n[0], depIndex1, ri );
    if( eVal!=0 ){
      int depIndex2;
      int eVal2 = evaluate( n[1], depIndex2, ri );
      if( eVal2!=0 ){
        depIndex = depIndex1>depIndex2 ? depIndex1 : depIndex2;
        return eVal==eVal2 ? 1 : -1;
      }
    }
    return 0;
  }else if( n.getKind()==ITE ){
    int depIndex1, depIndex2;
    int eVal = evaluate( n[0], depIndex1, ri );
    if( eVal==0 ){
      //evaluate children to see if they are the same value
      int eval1 = evaluate( n[1], depIndex1, ri );
      if( eval1!=0 ){
        int eval2 = evaluate( n[1], depIndex2, ri );
        if( eval1==eval2 ){
          depIndex = depIndex1>depIndex2 ? depIndex1 : depIndex2;
          return eval1;
        }
      }
    }else{
      int eValT = evaluate( n[eVal==1 ? 1 : 2], depIndex2, ri );
      depIndex = depIndex1>depIndex2 ? depIndex1 : depIndex2;
      return eValT;
    }
    return 0;
  }else if( n.getKind()==FORALL ){
    return 0;
  }else{
    //Debug("fmf-eval-debug") << "Evaluate literal " << n << std::endl;
    int retVal = 0;
    depIndex = ri->getNumTerms()-1;
    Node val = evaluateTerm( n, depIndex, ri );
    if( !val.isNull() ){
      if( areEqual( val, d_true ) ){
        retVal = 1;
      }else if( areEqual( val, d_false ) ){
        retVal = -1;
      }else{
        if( val.getKind()==EQUAL ){
          if( areEqual( val[0], val[1] ) ){
            retVal = 1;
          }else if( areDisequal( val[0], val[1] ) ){
            retVal = -1;
          }
        }
      }
    }
    if( retVal!=0 ){
      Debug("fmf-eval-debug") << "Evaluate literal: return " << retVal << ", depIndex = " << depIndex << std::endl;
    }else{
      Trace("fmf-eval-amb") << "Neither true nor false : " << n << std::endl;
      Trace("fmf-eval-amb") << "   value : " << val << std::endl;
    }
    return retVal;
  }
}

Node FirstOrderModelIG::evaluateTerm( Node n, int& depIndex, RepSetIterator* ri ){
  //Message() << "Eval term " << n << std::endl;
  Node val;
  depIndex = ri->getNumTerms()-1;
  //check the type of n
  if( n.getKind()==INST_CONSTANT ){
    int v = n.getAttribute(InstVarNumAttribute());
    depIndex = ri->getIndexOrder( v );
    val = ri->getCurrentTerm( v );
  }else if( n.getKind()==ITE ){
    int depIndex1, depIndex2;
    int eval = evaluate( n[0], depIndex1, ri );
    if( eval==0 ){
      //evaluate children to see if they are the same
      Node val1 = evaluateTerm( n[ 1 ], depIndex1, ri );
      Node val2 = evaluateTerm( n[ 2 ], depIndex2, ri );
      if( val1==val2 ){
        val = val1;
        depIndex = depIndex1>depIndex2 ? depIndex1 : depIndex2;
      }else{
        return Node::null();
      }
    }else{
      val = evaluateTerm( n[ eval==1 ? 1 : 2 ], depIndex2, ri );
      depIndex = depIndex1>depIndex2 ? depIndex1 : depIndex2;
    }
  }else{
    std::vector< int > children_depIndex;
    //default term evaluate : evaluate all children, recreate the value
    val = evaluateTermDefault( n, depIndex, children_depIndex, ri );
    Trace("fmf-eval-debug") << "Evaluate term, value from " << n << " is " << val << std::endl;
    if( !val.isNull() ){
      bool setVal = false;
      //custom ways of evaluating terms
      if( n.getKind()==APPLY_UF ){
        Node op = n.getOperator();
        //Debug("fmf-eval-debug") << "Evaluate term " << n << " (" << gn << ")" << std::endl;
        //if it is a defined UF, then consult the interpretation
        if( d_uf_model_tree.find( op )!=d_uf_model_tree.end() ){
          int argDepIndex = 0;
          //make the term model specifically for n
          makeEvalUfModel( n );
          //now, consult the model
          if( d_eval_uf_use_default[n] ){
            Trace("fmf-eval-debug") << "get default" << std::endl;
            val = d_uf_model_tree[ op ].getValue( this, val, argDepIndex );
          }else{
            Trace("fmf-eval-debug") << "get uf model" << std::endl;
            val = d_eval_uf_model[ n ].getValue( this, val, argDepIndex );
          }
          //Debug("fmf-eval-debug") << "Evaluate term " << n << " (" << gn << ")" << std::endl;
          //d_eval_uf_model[ n ].debugPrint("fmf-eval-debug", d_qe );
          Assert( !val.isNull() );
          //recalculate the depIndex
          depIndex = -1;
          for( int i=0; i<argDepIndex; i++ ){
            int index = d_eval_uf_use_default[n] ? i : d_eval_term_index_order[n][i];
            Debug("fmf-eval-debug") << "Add variables from " << index << "..." << std::endl;
            if( children_depIndex[index]>depIndex ){
              depIndex = children_depIndex[index];
            }
          }
          setVal = true;
        }else{
          Trace("fmf-eval-debug") << "No model." << std::endl;
        }
      }
      //if not set already, rewrite and consult model for interpretation
      if( !setVal ){
        val = Rewriter::rewrite( val );
        if( !val.isConst() ){
          return Node::null();
        }
      }
      Trace("fmf-eval-debug") << "Evaluate term " << n << " = ";
      Trace("fmf-eval-debug") << getRepresentative(val);
      Trace("fmf-eval-debug") << " (term " << val << "), depIndex = " << depIndex << std::endl;
    }
  }
  return val;
}

Node FirstOrderModelIG::evaluateTermDefault( Node n, int& depIndex, std::vector< int >& childDepIndex, RepSetIterator* ri ){
  depIndex = -1;
  if( n.getNumChildren()==0 ){
    return n;
  }else{
    bool isInterp = n.getKind()!=APPLY_UF;
    //first we must evaluate the arguments
    std::vector< Node > children;
    if( n.getMetaKind()==kind::metakind::PARAMETERIZED ){
      children.push_back( n.getOperator() );
    }
    //for each argument, calculate its value, and the variables its value depends upon
    for( int i=0; i<(int)n.getNumChildren(); i++ ){
      childDepIndex.push_back( -1 );
      Node nn = evaluateTerm( n[i], childDepIndex[i], ri );
      if( nn.isNull() ){
        depIndex = ri->getNumTerms()-1;
        return nn;
      }else{
        if( childDepIndex[i]>depIndex ){
          depIndex = childDepIndex[i];
        }
        if( isInterp ){
          if( !nn.isConst() ) {
            nn = getRepresentative( nn );
          }
        }
        children.push_back( nn );
      }
    }
    //recreate the value
    Node val = NodeManager::currentNM()->mkNode( n.getKind(), children );
    return val;
  }
}

void FirstOrderModelIG::makeEvalUfModel( Node n ){
  if( d_eval_uf_model.find( n )==d_eval_uf_model.end() ){
    makeEvalUfIndexOrder( n );
    if( !d_eval_uf_use_default[n] ){
      Node op = n.getOperator();
      d_eval_uf_model[n] = uf::UfModelTree( op, d_eval_term_index_order[n] );
      d_uf_model_gen[op].makeModel( this, d_eval_uf_model[n] );
      //Debug("fmf-index-order") << "Make model for " << n << " : " << std::endl;
      //d_eval_uf_model[n].debugPrint( std::cout, d_qe->getModel(), 2 );
    }
  }
}

struct sortGetMaxVariableNum {
  std::map< Node, int > d_max_var_num;
  int computeMaxVariableNum( Node n ){
    if( n.getKind()==INST_CONSTANT ){
      return n.getAttribute(InstVarNumAttribute());
    }else if( TermUtil::hasInstConstAttr(n) ){
      int maxVal = -1;
      for( int i=0; i<(int)n.getNumChildren(); i++ ){
        int val = getMaxVariableNum( n[i] );
        if( val>maxVal ){
          maxVal = val;
        }
      }
      return maxVal;
    }else{
      return -1;
    }
  }
  int getMaxVariableNum( Node n ){
    std::map< Node, int >::iterator it = d_max_var_num.find( n );
    if( it==d_max_var_num.end() ){
      int num = computeMaxVariableNum( n );
      d_max_var_num[n] = num;
      return num;
    }else{
      return it->second;
    }
  }
  bool operator() (Node i,Node j) { return (getMaxVariableNum(i)<getMaxVariableNum(j));}
};

void FirstOrderModelIG::makeEvalUfIndexOrder( Node n ){
  if( d_eval_term_index_order.find( n )==d_eval_term_index_order.end() ){
    //sort arguments in order of least significant vs. most significant variable in default ordering
    std::map< Node, std::vector< int > > argIndex;
    std::vector< Node > args;
    for( int i=0; i<(int)n.getNumChildren(); i++ ){
      if( argIndex.find( n[i] )==argIndex.end() ){
        args.push_back( n[i] );
      }
      argIndex[n[i]].push_back( i );
    }
    sortGetMaxVariableNum sgmvn;
    std::sort( args.begin(), args.end(), sgmvn );
    for( int i=0; i<(int)args.size(); i++ ){
      for( int j=0; j<(int)argIndex[ args[i] ].size(); j++ ){
        d_eval_term_index_order[n].push_back( argIndex[ args[i] ][j] );
      }
    }
    bool useDefault = true;
    for( int i=0; i<(int)d_eval_term_index_order[n].size(); i++ ){
      if( i!=d_eval_term_index_order[n][i] ){
        useDefault = false;
        break;
      }
    }
    d_eval_uf_use_default[n] = useDefault;
    Debug("fmf-index-order") << "Will consider the following index ordering for " << n << " : ";
    for( int i=0; i<(int)d_eval_term_index_order[n].size(); i++ ){
      Debug("fmf-index-order") << d_eval_term_index_order[n][i] << " ";
    }
    Debug("fmf-index-order") << std::endl;
  }
}

FirstOrderModelFmc::FirstOrderModelFmc(QuantifiersEngine * qe, context::Context* c, std::string name) :
FirstOrderModel(qe, c, name){

}

FirstOrderModelFmc::~FirstOrderModelFmc()
{
  for(std::map<Node, Def*>::iterator i = d_models.begin(); i != d_models.end(); ++i) {
    delete (*i).second;
  }
}

void FirstOrderModelFmc::processInitialize( bool ispre ) {
  if( ispre ){
    for( std::map<Node, Def * >::iterator it = d_models.begin(); it != d_models.end(); ++it ){
      it->second->reset();
    }
  }
}

void FirstOrderModelFmc::processInitializeModelForTerm(Node n) {
  if( n.getKind()==APPLY_UF ){
    if( d_models.find(n.getOperator())==d_models.end()) {
      d_models[n.getOperator()] = new Def;
    }
  }
}


bool FirstOrderModelFmc::isStar(Node n) {
  //return n==getStar(n.getType());
  return n.getAttribute(IsStarAttribute());
}

Node FirstOrderModelFmc::getStar(TypeNode tn) {
  std::map<TypeNode, Node >::iterator it = d_type_star.find( tn );
  if( it==d_type_star.end() ){
    Node st = NodeManager::currentNM()->mkSkolem( "star", tn, "skolem created for full-model checking" );
    d_type_star[tn] = st;
    st.setAttribute(IsStarAttribute(), true );
    return st;
  }
  return it->second;
}

Node FirstOrderModelFmc::getFunctionValue(Node op, const char* argPrefix ) {
  Trace("fmc-model") << "Get function value for " << op << std::endl;
  TypeNode type = op.getType();
  std::vector< Node > vars;
  for( size_t i=0; i<type.getNumChildren()-1; i++ ){
    std::stringstream ss;
    ss << argPrefix << (i+1);
    Node b = NodeManager::currentNM()->mkBoundVar( ss.str(), type[i] );
    vars.push_back( b );
  }
  Node boundVarList = NodeManager::currentNM()->mkNode(kind::BOUND_VAR_LIST, vars);
  Node curr;
  for( int i=(d_models[op]->d_cond.size()-1); i>=0; i--) {
    Node v = d_models[op]->d_value[i];
    Trace("fmc-model-func") << "Value is : " << v << std::endl;
    Assert( v.isConst() );
    /*
    if( !hasTerm( v ) ){
      //can happen when the model basis term does not exist in ground assignment
      TypeNode tn = v.getType();
      //check if it is a constant introduced as a representative not existing in the model's equality engine
      if( !d_rep_set.hasRep( tn, v ) ){
        if( d_rep_set.d_type_reps.find( tn )!=d_rep_set.d_type_reps.end() && !d_rep_set.d_type_reps[ tn ].empty() ){
          v = d_rep_set.d_type_reps[tn][ d_rep_set.d_type_reps[tn].size()-1 ];
        }else{
          //can happen for types not involved in quantified formulas
          Trace("fmc-model-func") << "No type rep for " << tn << std::endl;
          v = d_qe->getTermUtil()->getEnumerateTerm( tn, 0 );
        }
        Trace("fmc-model-func") << "No term, assign " << v << std::endl;
      }
    }
    v = getRepresentative( v );
    */
    if( curr.isNull() ){
      Trace("fmc-model-func") << "base : " << v << std::endl;
      curr = v;
    }else{
      //make the condition
      Node cond = d_models[op]->d_cond[i];
      Trace("fmc-model-func") << "...cond : " << cond << std::endl;
      std::vector< Node > children;
      for( unsigned j=0; j<cond.getNumChildren(); j++) {
        TypeNode tn = vars[j].getType();
        if (!isStar(cond[j]))
        {
          Node c = getRepresentative( cond[j] );
          c = getRepresentative( c );
          children.push_back( NodeManager::currentNM()->mkNode( EQUAL, vars[j], c ) );
        }
      }
      Assert( !children.empty() );
      Node cc = children.size()==1 ? children[0] : NodeManager::currentNM()->mkNode( AND, children );

      Trace("fmc-model-func") << "condition : " << cc << ", value : " << v << std::endl;
      curr = NodeManager::currentNM()->mkNode( ITE, cc, v, curr );
    }
  }
  Trace("fmc-model") << "Made " << curr << " for " << op << std::endl;
  curr = Rewriter::rewrite( curr );
  return NodeManager::currentNM()->mkNode(kind::LAMBDA, boundVarList, curr);
}

FirstOrderModelAbs::FirstOrderModelAbs(QuantifiersEngine * qe, context::Context* c, std::string name) :
FirstOrderModel(qe, c, name) {

}

FirstOrderModelAbs::~FirstOrderModelAbs()
{
  for(std::map<Node, AbsDef*>::iterator i = d_models.begin(); i != d_models.end(); ++i) {
    delete (*i).second;
  }
}

void FirstOrderModelAbs::processInitialize( bool ispre ) {
  if( !ispre ){
    Trace("ambqi-debug") << "Process initialize" << std::endl;
    for( std::map<Node, AbsDef * >::iterator it = d_models.begin(); it != d_models.end(); ++it ) {
      Node op = it->first;
      TypeNode tno = op.getType();
      Trace("ambqi-debug") << "  Init " << op << " " << tno << std::endl;
      for( unsigned i=0; i<tno.getNumChildren(); i++) {
        //make sure a representative of the type exists
        if( !d_rep_set.hasType( tno[i] ) ){
          Node e = getSomeDomainElement( tno[i] );
          Trace("ambqi-debug") << "  * Initialize type " << tno[i] << ", add ";
          Trace("ambqi-debug") << e << " " << e.getType() << std::endl;
          //d_rep_set.add( e );
        }
      }
    }
  }
}

unsigned FirstOrderModelAbs::getRepresentativeId( TNode n ) {
  TNode r = getUsedRepresentative( n );
  std::map< TNode, unsigned >::iterator it = d_rep_id.find( r );
  if( it!=d_rep_id.end() ){
    return it->second;
  }else{
    return 0;
  }
}

TNode FirstOrderModelAbs::getUsedRepresentative( TNode n ) {
  if( hasTerm( n ) ){
    if( n.getType().isBoolean() ){
      return areEqual(n, d_true) ? d_true : d_false;
    }else{
      return getRepresentative( n );
    }
  }else{
    Trace("qint-debug") << "Get rep " << n << " " << n.getType() << std::endl;
    Assert( d_rep_set.hasType( n.getType() ) && !d_rep_set.d_type_reps[n.getType()].empty() );
    return d_rep_set.d_type_reps[n.getType()][0];
  }
}

Node FirstOrderModelAbs::getFunctionValue(Node op, const char* argPrefix ) {
  if( d_models_valid[op] ){
    Trace("ambqi-debug") << "Get function value for " << op << std::endl;
    TypeNode type = op.getType();
    std::vector< Node > vars;
    for( size_t i=0; i<type.getNumChildren()-1; i++ ){
      std::stringstream ss;
      ss << argPrefix << (i+1);
      Node b = NodeManager::currentNM()->mkBoundVar( ss.str(), type[i] );
      vars.push_back( b );
    }
    Node boundVarList = NodeManager::currentNM()->mkNode(kind::BOUND_VAR_LIST, vars);
    Node curr = d_models[op]->getFunctionValue( this, op, vars );
    Node fv = NodeManager::currentNM()->mkNode(kind::LAMBDA, boundVarList, curr);
    Trace("ambqi-debug") << "Return " << fv << std::endl;
    return fv;
  }else{

  }
  return Node::null();
}

void FirstOrderModelAbs::processInitializeModelForTerm( Node n ) {
  if( n.getKind()==APPLY_UF || n.getKind()==VARIABLE || n.getKind()==SKOLEM ){
    Node op = n.getKind()==APPLY_UF ? n.getOperator() : n;
    if( d_models.find(op)==d_models.end()) {
      Trace("abmqi-debug") << "init model for " << op << std::endl;
      d_models[op] = new AbsDef;
      d_models_valid[op] = false;
    }
  }
}

void FirstOrderModelAbs::collectEqVars( TNode q, TNode n, std::map< int, bool >& eq_vars ) {
  for( unsigned i=0; i<n.getNumChildren(); i++ ){
    if( n.getKind()==EQUAL && n[i].getKind()==BOUND_VARIABLE ){
      int v = getVariableId( q, n[i] );
      Assert( v>=0 && v<(int)q[0].getNumChildren() );
      eq_vars[v] = true;
    }
    collectEqVars( q, n[i], eq_vars );
  }
}

void FirstOrderModelAbs::processInitializeQuantifier( Node q ) {
  if( d_var_order.find( q )==d_var_order.end() ){
    std::map< int, bool > eq_vars;
    for( unsigned i=0; i<q[0].getNumChildren(); i++ ){
      eq_vars[i] = false;
    }
    collectEqVars( q, q[1], eq_vars );
    for( unsigned r=0; r<2; r++ ){
      for( std::map< int, bool >::iterator it = eq_vars.begin(); it != eq_vars.end(); ++it ){
        if( it->second==(r==1) ){
          d_var_index[q][it->first] = d_var_order[q].size();
          d_var_order[q].push_back( it->first );
        }
      }
    }
  }
}

Node FirstOrderModelAbs::getVariable( Node q, unsigned i ) {
  return q[0][d_var_order[q][i]];
}

} /* CVC4::theory::quantifiers namespace */
} /* CVC4::theory namespace */
} /* CVC4 namespace */
