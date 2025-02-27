/*
 * Copyright 2020-2023 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ForLoop.h"

#include "astutil.h"
#include "AstVisitor.h"
#include "build.h"
#include "DeferStmt.h"
#include "driver.h"
#include "stringutil.h"

#include <algorithm>

/************************************ | *************************************
*                                                                           *
* Helper functions to optimize anonymous range iteration                    *
*                                                                           *
************************************* | ************************************/

/*
 * Attempts to replace iteration over simple anonymous ranges with calls to
 * direct iterators that take low, high and stride as arguments. This is to
 * avoid the cost of constructing ranges, and if the stride is known at compile
 * time, provide a more optimized iterator that uses "<, <=, >, or >=" as the
 * relational operator.
 *
 * This is only meant to replace anonymous range iteration for "simple" ranges.
 * Simple means it's a range of the form "low..high", "low..high by stride", or
 * "low..#count". Anything more complex is ignored with the thinking that this
 * should optimize the most common range iterators, but it could be expanded to
 * handle more cases.
 *
 * An alternative is to update scalar replacement of aggregates to work on
 * ranges, which should be able to achieve similar results as this optimization
 * while handling all ranges, including non-anonymous ranges.
 *
 * This function will optimize things like:
 * - "for i in 1..10"
 * - "for i in 1..10+1"
 * - "var lo=1, hi=10;for i in lo..hi"
 * - "for i in 1..10 by 2"
 * - "for i in 1..#10"
 * - "for (i, j) in zip(1..10 by 2, 1..10 by -2)"
 * - "for (i, j) in zip(A, 1..10 by 2)" // will optimize range iter still
 * - "coforall i in 1..10 by 2"         // works for coforalls as well
 *
 * Will not optimize ranges like:
 * - "for in in (1..)"             // doesn't handle unbounded ranges
 * - "for i in 1..10 by 2 by 2"    // doesn't handle more than one by operator
 * - "for i in 1..10 align 2"      // doesn't handle align operator
 * - "for i in (1..10)#2"          // doesn't handle bounded counted ranges
 * - "for i in 1..#10 by 2"        // doesn't handle strided and counted ranges
 * - "var r = 1..10"; for i in r"  // not an anonymous range
 * - "forall i in 1..10"           // doesn't get applied to foralls
 *
 * Note that this function is pretty fragile because it relies on names of
 * functions/iterators as well as the arguments and order of those
 * functions/iterators but there's not really a way around it this early in
 * compilation. If the iterator can't be replaced, it is left unchanged.
 */
static void tryToReplaceWithDirectRangeIterator(Expr* iteratorExpr)
{
  if (fNoOptimizeRangeIteration)
    return;
  if (CallExpr* call = toCallExpr(iteratorExpr))
  {
    CallExpr* range = NULL;
    Expr* stride = NULL;
    Expr* count = NULL;
    // grab the stride if we have a strided range
    if (call->isNamed("chpl_by"))
    {
      range = toCallExpr(call->get(1)->copy());
      stride = toExpr(call->get(2)->copy());
    }
    // or grab the count if we have a counted range
    else if (call->isNamed("#"))
    {
      range = toCallExpr(call->get(1)->copy());
      count = toExpr(call->get(2)->copy());
    }
    // or assume the call is the range (checked below) and leave stride null
    else
    {
      range = call;
      // stride remains NULL
    }

    //
    // see if we're looking at a range builder. The builder is iteratable since
    // range has these() iterators
    //

    if (!range) {
      return;
    }

    bool fullyBounded = range->isNamed("chpl_build_bounded_range");
    bool lowBounded = range->isNamed("chpl_build_low_bounded_range");

    if (!fullyBounded && !lowBounded) {
      return;
    }

    if (!stride && !count && fullyBounded) {
      // replace fully bounded and non-strided range with a direct range
      // iter. e.g. replace:
      //
      //   `low..high`
      //
      // with:
      //
      // `chpl_direct_range_iter(low, high)`
      Expr* low = range->get(1)->copy();
      Expr* high = range->get(2)->copy();
      iteratorExpr->replace(new CallExpr("chpl_direct_range_iter", low, high));

    } else if (stride && !count && fullyBounded) {
      // replace fully bounded and strided range with a direct range
      // iter. e.g. replace:
      //
      //   `low..high by stride`
      //
      // with:
      //
      // `chpl_direct_strided_range_iter(low, high, stride)`
      Expr* low = range->get(1)->copy();
      Expr* high = range->get(2)->copy();
      iteratorExpr->replace(new CallExpr("chpl_direct_strided_range_iter",
                                         low, high, stride));

    } else if (!stride && count && lowBounded) {
      // replace counted, low bounded range with unit stride with an equivalent
      // direct range iter. e.g. replace:
      //
      //   `low..#count`
      //
      // with:
      //
      //   `chpl_direct_counted_range_iter(low, count)`
      Expr* low = range->get(1)->copy();
      iteratorExpr->replace(new CallExpr("chpl_direct_counted_range_iter",
                            low, count));

    }
  }
}

/************************************ | *************************************
*                                                                           *
* Factory methods for the Parser                                            *
*                                                                           *
************************************* | ************************************/

BlockStmt* ForLoop::doBuildForLoop(Expr*      indices,
                          Expr*      iteratorExpr,
                          CallExpr*  intents,
                          BlockStmt* body,
                          LLVMMetadataList attrs,
                          bool       coforall,
                          bool       zippered,
                          bool       isLoweredForall,
                          bool       isForExpr,
                          bool       isForeach)
{
  VarSymbol*   index         = newTemp("_indexOfInterest");
  VarSymbol*   iterator      = newTemp("_iterator");
  CallExpr*    iterInit      = 0;
  CallExpr*    iterMove      = 0;
  ForLoop*     loop          = new ForLoop(index, iterator, body,
                                           zippered, isLoweredForall,
                                           isForExpr);
  LabelSymbol* continueLabel = new LabelSymbol("_continueLabel");
  LabelSymbol* breakLabel    = new LabelSymbol("_breakLabel");
  BlockStmt*   retval        = new BlockStmt();

  iterator->addFlag(FLAG_EXPR_TEMP);

  loop->setAdditionalLLVMMetadata(attrs);

  if (isForeach) {
    loop->orderIndependentSet(true);
  }

  // Unzippered loop, treat all objects (including tuples) the same
  if (zippered == false) {
    iterInit = new CallExpr(PRIM_MOVE, iterator, new CallExpr("_getIterator",    iteratorExpr));
    // try to optimize anonymous range iteration
    tryToReplaceWithDirectRangeIterator(iteratorExpr);
  }
  // Zippered loop: Expand args to a tuple with an iterator for each element.
  else {
    CallExpr* zipExpr = toCallExpr(iteratorExpr);
    if (zipExpr && zipExpr->isPrimitive(PRIM_ZIP)) {
      // The PRIM_ZIP indicates this is a new-style zip() AST.
      // Expand arguments to a tuple with appropriate iterators for each value.
      //
      // Specifically, change:
      //    zip(a, b, c,  ...)
      // into the tuple:
      //    (_getIterator(a), _getIterator(b), _getIterator(c), ...)
      //
      // (ultimately, we will probably want to make this style of
      // rewrite into a utility function for the other get*Zip
      // functions as we convert parallel loops over to use PRIM_ZIP).
      //
      zipExpr->primitive = NULL;   // remove the primitive

      // If there's just one argument...
      if (zipExpr->argList.length == 1) {
        Expr* zipArg = zipExpr->argList.only();
        CallExpr* zipArgCall = toCallExpr(zipArg);

        // ...and it is a tuple expansion '(...t)' then remove the
        // tuple expansion primitive and simply pass the tuple itself
        // to _getIteratorZip().  This will not require any more
        // tuples than the user introduced themselves.
        //
        if (zipArgCall && zipArgCall->isPrimitive(PRIM_TUPLE_EXPAND)) {
          zipExpr->baseExpr = new UnresolvedSymExpr("_getIteratorZip");
          Expr* tupleArg = zipArgCall->argList.only();
          tupleArg->remove();
          zipArgCall->replace(tupleArg);
        } else {
          // ...otherwise, make the expression into a _getIterator()
          // call
          zipExpr->baseExpr = new UnresolvedSymExpr("_getIterator");
          // try to optimize anonymous range iteration
          tryToReplaceWithDirectRangeIterator(zipArg);
        }
      } else {
        //
        // Otherwise, if there's more than one argument, build up the
        // tuple by applying _getIterator() to each element.
        //
        zipExpr->baseExpr = new UnresolvedSymExpr("_build_tuple");
        Expr* arg = zipExpr->argList.first();
        while (arg) {
          Expr* next = arg->next;
          Expr* argCopy = arg->copy();
          arg->replace(new CallExpr("_getIterator", argCopy));
          // try to optimize anonymous range iteration
          tryToReplaceWithDirectRangeIterator(argCopy);
          arg = next;
        }
      }
      iterInit = new CallExpr(PRIM_MOVE, iterator, zipExpr);
      assert(zipExpr == iteratorExpr);
    } else {
      //
      // This is an old-style zippered loop so handle it in the old style
      //
      iterInit = new CallExpr(PRIM_MOVE, iterator,
                              new CallExpr("_getIteratorZip", iteratorExpr));

      // try to optimize anonymous range iteration
      if (CallExpr* call = toCallExpr(iteratorExpr))
        if (call->isNamedAstr(astrBuildTuple))
          for_actuals(actual, call)
            tryToReplaceWithDirectRangeIterator(actual);
    }
  }

  index->addFlag(FLAG_INDEX_OF_INTEREST);

  iterMove = new CallExpr(PRIM_MOVE, index, new CallExpr("iteratorIndex", iterator));

  if (indices == 0) {
    VarSymbol* var = new VarSymbol("chpl__elidedIdx");
    indices = new DefExpr(var);
  }

  checkIndices(indices);

  destructureIndices(loop, indices, new SymExpr(index), coforall);

  if (coforall)
    index->addFlag(FLAG_COFORALL_INDEX_VAR);

  loop->mContinueLabel = continueLabel;
  loop->mBreakLabel    = breakLabel;

  // Transfer the DefExprs of the intent variables (ShadowVarSymbols).
  if (intents) {
    while (Expr* src = intents->argList.head)
      loop->shadowVariables().insertAtTail(src->remove());
  }

  loop->insertAtTail(new DefExpr(continueLabel));

  retval->insertAtTail(new DefExpr(index));
  retval->insertAtTail(new DefExpr(iterator));

  retval->insertAtTail(iterInit);
  retval->insertAtTail(new DeferStmt(new CallExpr("_freeIterator", iterator)));
  retval->insertAtTail(new BlockStmt(iterMove, BLOCK_TYPE));

  retval->insertAtTail(loop);

  retval->insertAtTail(new DefExpr(breakLabel));

  return retval;
}

BlockStmt* ForLoop::buildForLoop(Expr*      indices,
                                 Expr*      iteratorExpr,
                                 BlockStmt* body,
                                 bool       zippered,
                                 bool       isForExpr,
                                 LLVMMetadataList attrs)
{
  return doBuildForLoop(indices, iteratorExpr,
                        /* intents */ nullptr,
                        body,
                        attrs,
                        /* coforall */ false,
                        zippered,
                        /* isLoweredForall */ false,
                        isForExpr,
                        /* isForeach */ false);
}

BlockStmt* ForLoop::buildForeachLoop(Expr*      indices,
                                     Expr*      iteratorExpr,
                                     CallExpr*  intents,
                                     BlockStmt* body,
                                     bool       zippered,
                                     bool       isForExpr,
                                     LLVMMetadataList attrs)

{
  return doBuildForLoop(indices, iteratorExpr, intents, body,
                        attrs,
                        /* coforall */ false,
                        zippered,
                        /* isLoweredForall */ false,
                        isForExpr,
                        /* isForeach */ true);
}

BlockStmt* ForLoop::buildCoforallLoop(Expr*      indices,
                                      Expr*      iteratorExpr,
                                      BlockStmt* body,
                                      bool       zippered,
                                      LLVMMetadataList attrs)
{
  return doBuildForLoop(indices, iteratorExpr,
                        /* intents */ nullptr,
                        body,
                        attrs,
                        /* coforall */ true,
                        zippered,
                        /* isLoweredForall */ false,
                        /* isForExpr */ false,
                        /* isForeach */ false);
}


BlockStmt* ForLoop::buildLoweredForallLoop(Expr*      indices,
                                           Expr*      iteratorExpr,
                                           BlockStmt* body,
                                           bool       zippered,
                                           bool       isForExpr,
                                           LLVMMetadataList attrs)
{
  return doBuildForLoop(indices, iteratorExpr,
                        /* intents */ nullptr,
                        body,
                        attrs,
                        /* coforall */ false,
                        zippered,
                        /* isLoweredForall */ true,
                        isForExpr,
                        /* isForeach */ true);
}


/************************************ | *************************************
*                                                                           *
* Instance methods                                                          *
*                                                                           *
************************************* | ************************************/

ForLoop::ForLoop() : LoopStmt(0)
{
  mIndex    = 0;
  mIterator = 0;
  mZippered = false;
  mLoweredForall = false;
  mIsForExpr = false;
}

ForLoop::ForLoop(VarSymbol* index,
                 VarSymbol* iterator,
                 BlockStmt* initBody,
                 bool       zippered,
                 bool       isLoweredForall,
                 bool       isForExpr) : LoopStmt(initBody)
{
  mIndex    = new SymExpr(index);
  mIterator = new SymExpr(iterator);
  mZippered = zippered;
  mLoweredForall = isLoweredForall;
  mIsForExpr = isForExpr;
  fShadowVars.parent = this;
}

ForLoop* ForLoop::copyInner(SymbolMap* map)
{
  ForLoop*   retval         = new ForLoop();

  retval->astloc            = astloc;
  retval->blockTag          = blockTag;

  retval->mBreakLabel       = mBreakLabel;
  retval->mContinueLabel    = mContinueLabel;
  retval->mOrderIndependent = mOrderIndependent;
  retval->mLLVMMetadataList = mLLVMMetadataList;

  retval->mIndex            = mIndex->copy(map, true),
  retval->mIterator         = mIterator->copy(map, true);
  retval->mZippered         = mZippered;

  // MPF 2020-01-21: It seems it should also copy mLoweredForall,
  // but doing so causes problems in lowerIterators.
  retval->mIsForExpr        = mIsForExpr;

  retval->userLabel         = userLabel;

  for_alist(expr, body)
    retval->insertAtTail(expr->copy(map, true));

  return retval;
}

BlockStmt* ForLoop::copyBody()
{
  SymbolMap map;

  return copyBody(&map);
}

BlockStmt* ForLoop::copyBody(SymbolMap* map)
{
  BlockStmt* retval = new BlockStmt();

  retval->astloc   = astloc;
  retval->blockTag = blockTag;

  for_alist(expr, body)
    retval->insertAtTail(expr->copy(map, true));

  update_symbols(retval, map);

  return retval;
}


void ForLoop::copyBodyHelper(Expr* beforeHere, int64_t i, SymbolMap* map,
                             Symbol* continueSym)
{
  // Replace the continue label with a per-iteration label
  // that is at the end of that iteration.
  LabelSymbol* continueLabel = new
    LabelSymbol(astr("_continueLabel", istr(i)));
  Expr* defContinueLabel = new DefExpr(continueLabel);

  beforeHere->insertBefore(defContinueLabel);

  map->put(continueSym, continueLabel);

  defContinueLabel->insertBefore(copyBody(map));
}

// TODO (Elliot 03/03/15): coforall loops are currently represented
// as ForLoops in the compiler. This is a start at distinguishing
// them. Note that for coforall loops, this method and isForLoop
// with both return true. Eventually CoforallLoop should become it's
// own class that shares a common parent with ForLoop.
bool ForLoop::isCoforallLoop() const
{
  return mIndex->symbol()->hasFlag(FLAG_COFORALL_INDEX_VAR);
}

bool ForLoop::isLoweredForallLoop() const
{
  return mLoweredForall;
}

bool ForLoop::isForExpr() const
{
  return mIsForExpr;
}

SymExpr* ForLoop::indexGet() const
{
  return mIndex;
}

SymExpr* ForLoop::iteratorGet() const
{
  return mIterator;
}

bool ForLoop::zipperedGet() const
{
  return mZippered;
}

CallExpr* ForLoop::blockInfoGet() const
{
  printf("Migration: ForLoop   %12d Unexpected call to blockInfoGet()\n", id);

  return 0;
}

CallExpr* ForLoop::blockInfoSet(CallExpr* expr)
{
  printf("Migration: ForLoop   %12d Unexpected call to blockInfoSet()\n", id);

  return 0;
}

bool ForLoop::deadBlockCleanup()
{
  bool retval = false;

  INT_ASSERT(false);

  return retval;
}

void ForLoop::verify()
{
  BlockStmt::verify();

  if (BlockStmt::blockInfoGet() != 0)
    INT_FATAL(this, "ForLoop::verify. blockInfo is not NULL");

  if (mIndex    == 0)
    INT_FATAL(this, "ForLoop::verify. index     is NULL");

  if (mIterator == 0)
    INT_FATAL(this, "ForLoop::verify. iterator  is NULL");

  if (useList   != 0)
    INT_FATAL(this, "ForLoop::verify. useList   is not NULL");

  if (byrefVars != 0)
    INT_FATAL(this, "ForLoop::verify. byrefVars is not NULL");
}

GenRet ForLoop::codegen()
{
  GenRet ret;

  INT_FATAL(this, "ForLoop::codegen This should be unreachable");

  return ret;
}

void ForLoop::accept(AstVisitor* visitor)
{
  if (visitor->enterForLoop(this) == true)
  {
    for_alist(next_ast, body)
      next_ast->accept(visitor);

    if (indexGet()    != 0)
      indexGet()->accept(visitor);

    if (iteratorGet() != 0)
      iteratorGet()->accept(visitor);

    if (useList)
      useList->accept(visitor);

    if (byrefVars)
      byrefVars->accept(visitor);

    visitor->exitForLoop(this);
  }
}

void ForLoop::replaceChild(Expr* oldAst, Expr* newAst)
{
  if (oldAst == mIndex)
  {
    SymExpr* se = toSymExpr(newAst);
    // Complain if the newAst is not NULL and cannot be converted to a SymExpr.
    INT_ASSERT(!newAst || se);
    mIndex = se;
  }
  else if (oldAst == mIterator)
  {
    SymExpr* se = toSymExpr(newAst);
    // Complain if the newAst is not NULL and cannot be converted to a SymExpr.
    INT_ASSERT(!newAst || se);
    mIterator = se;
  }
  else
    LoopStmt::replaceChild(oldAst, newAst);
}

Expr* ForLoop::getFirstExpr()
{
  Expr* retval = 0;

  if (mIndex         != 0)
    retval = mIndex;

  else if (mIterator != 0)
    retval = mIterator;

  else if (body.head != 0)
    retval = body.head->getFirstExpr();

  else
    retval = this;

  return retval;
}

Expr* ForLoop::getNextExpr(Expr* expr)
{
  Expr* retval = this;

  if (expr == mIndex && mIterator != NULL)
    retval = mIterator;

  else if (expr == mIndex    && body.head != NULL)
    retval = body.head->getFirstExpr();

  else if (expr == mIterator && body.head != NULL)
    retval = body.head->getFirstExpr();

  return retval;
}

bool ForLoop::isInductionVar(Symbol* sym) {
  return sym == mIndex->symbol();
}
