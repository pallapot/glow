/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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

#include "glow/Base/Tensor.h"
#include "glow/ExecutionEngine/ExecutionEngine2.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"
#include "glow/IR/IR.h"
#include "glow/IR/IRBuilder.h"
#include "glow/IR/Instrs.h"

#include "gtest/gtest.h"

using namespace glow;
using llvm::cast;

class GradCheckBase : public ::testing::TestWithParam<std::string> {
public:
  ExecutionEngine2 EET_{GetParam()};
  ExecutionEngine2 EEI_{GetParam()};
  std::vector<ExecutionEngine2 *> engines_;
  void SetUp() {
    engines_.push_back(&EEI_);
    engines_.push_back(&EET_);
  }
};

class InterpreterGrad : public GradCheckBase {};
class GradCheck : public GradCheckBase {};

/// \returns the regression loss for the tensor \p X with regard to \p Y.
float computeL2Loss(Tensor *X, Tensor *Y) {
  assert(X->dims() == Y->dims() && "Invalid input dims");
  auto xH = X->getHandle<>();
  auto yH = Y->getHandle<>();
  float loss = 0;

  for (size_t i = 0, e = X->size(); i < e; i++) {
    float dy = (xH.raw(i) - yH.raw(i));
    loss += 0.5 * dy * dy;
  }

  return loss;
}

/// \returns the error when comparing two grads: absolute or relative.
float gradDiff(float G1, float G2) {
  return std::min(std::abs(G1 - G2), std::abs(G1 - G2) / std::abs(G1 + G2 + 1));
}

Placeholder *getGrad(const VariableGradientsList &grads, Placeholder *V) {
  for (auto &p : grads) {
    if (p.first == V) {
      return p.second;
    }
  }
  return nullptr;
}

void allocateGrads(PlaceholderBindings &bindings,
                   const VariableGradientsList &grads) {
  for (auto &p : grads) {
    auto grad = p.second;
    bindings.allocate(grad);
  }
}

/// Performs gradient check by comparing analytical and numerical gradients.
/// Numeric grad is calculated based on: f(x-delta) and f(x+delta) values.
/// Analytical grad is based on the gradient output calculated during back
/// propagation.
///
/// \param EET Execution engine to compile/run network for training.
/// \param EEI Executiuon engine to compile/run network for inference.
/// \param bindings Placeholder bindings.
/// \param result Node that contains result of f(x).
/// \param inputVar Placeholder which gradient is assessed.
/// \param expVar Placeholder with expected value, only used during the
/// training. \param inputs Tensor for \p inputVar placeholder. \param outputs
/// Tensor for \p expVar placeholder. \param allowedError allowed delta between
/// analytical and numerical gradients.
void performGradCheck(ExecutionEngine2 &EET, ExecutionEngine2 &EEI,
                      PlaceholderBindings &bindings, Placeholder *result,
                      Placeholder *inputVar, Placeholder *expVar,
                      Tensor *inputs, Tensor *outputs, float delta,
                      float allowedError) {
  TrainingConfig TC;

  auto *F = EET.getModule().getFunction("main");

  // Allocate result, inputVar and expVar.
  bindings.allocate(result);
  bindings.allocate(inputVar);
  bindings.allocate(expVar);

  // This variable records the number of the next sample to be used for
  // training.
  size_t sampleCounter = 0;

  // Create a function that trains the network.
  auto *TF = glow::differentiate(F, TC);

  // Create a version of the network that records the gradients to some side
  // table instead of updating them.
  VariableGradientsList varGrads;
  auto *RF = glow::differentiate(F, TC, "record", &varGrads);
  auto rfName = RF->getName();
  auto tfName = TF->getName();
  EET.compile(CompilationMode::Train);
  bindings.allocate(EET.getModule().getPlaceholders());

  // The network might have variables, other than inputVar and expVar.
  // Train the network until other variables reach some stable local minimum.
  runBatch2(EET, bindings, 300, sampleCounter, {inputVar, expVar},
            {inputs, outputs}, tfName);

  // Clear the gradients of the first layer.
  auto gradVar = getGrad(varGrads, inputVar);
  auto gradVarTensor = bindings.get(gradVar);
  gradVarTensor->zero();

  // Train the network just once to record the values of gradient for inputVar.
  runBatch2(EET, bindings, 1, sampleCounter, {inputVar, expVar},
            {inputs, outputs}, rfName);
  // Compile the original network in inference mode.
  EEI.compile(CompilationMode::Infer);
  PlaceholderBindings inferBindings;
  inferBindings.allocate(EEI.getModule().getPlaceholders());
  // Copy values to inferenceBindings. This is needed for any placeholders that
  // were initialized during function creation.
  for (auto PH : EEI.getModule().getPlaceholders()) {
    bindings.copyToTarget(PH->getName(), inferBindings);
  }
  auto resultTensor =
      inferBindings.get(inferBindings.getPlaceholderByName(result->getName()));

  auto analyticalGradsH = gradVarTensor->getHandle();
  auto inputsH = inputs->getHandle<>();
  bool allZeroGradients = true;
  auto inferInput = inferBindings.getPlaceholderByName(inputVar->getName());

  for (size_t i = 0; i < analyticalGradsH.size(); i++) {
    auto old = inputsH.raw(i);
    // Calculate f(x+e):
    inputsH.raw(i) = old + delta;
    updateInputPlaceholders2(inferBindings, {inferInput}, {inputs});
    EEI.run(inferBindings);
    auto plusLoss = computeL2Loss(outputs, resultTensor);

    // Calculate f(x-e):
    inputsH.raw(i) = old - delta;
    updateInputPlaceholders2(inferBindings, {inferInput}, {inputs});
    EEI.run(inferBindings);

    auto minusLoss = computeL2Loss(outputs, resultTensor);

    // Restore value back.
    inputsH.raw(i) = old;

    auto numericGrad = (plusLoss - minusLoss) / (2 * delta);
    auto analyticalGrad = analyticalGradsH.raw(i);
    auto err = gradDiff(analyticalGrad, numericGrad);

    if (numericGrad != 0.0) {
      allZeroGradients = false;
    }
    // Make sure that the analytical and numerical gradients agree.
    EXPECT_LE(err, allowedError);
  }

  // Make sure that some gradients are non-zero. If all gradients are zero,
  // it means that the input doesn't affect the output, which also means
  // that the operations being test is totally ignored.
  EXPECT_FALSE(allZeroGradients);
}

TEST_P(InterpreterGrad, gradientCheckConcat) {
  PlaceholderBindings bindings;
  Placeholder *A, *Exp, *B;
  SaveNode *result;
  size_t numOutputElem = 20;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    Function *F = mod.createFunction("main");

    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem / 2}, "A",
                              false);
    B = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem / 2}, "B",
                              false);

    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "exp",
                                false);
    Node *O = F->createConcat("concat", {A, B}, 1);
    O = F->createRegression("reg", O, Exp);
    result = F->createSave("ret", O);
  }
  bindings.allocate(B)->zero();
  Tensor inputs(ElemKind::FloatTy, {{1, numOutputElem / 2}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &trainingMod = EET_.getModule();
  inputsH.randomize(-1, 1, trainingMod.getPRNG());
  outputsH.randomize(-1, 1, trainingMod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckMatMul) {
  PlaceholderBindings Bindings;
  Placeholder *A, *Exp, *B;
  SaveNode *Result;
  size_t NumDim = 10;
  for (auto *EE : engines_) {
    auto &Mod = EE->getModule();
    Bindings.clear();
    Function *F = Mod.createFunction("main");

    A = Mod.createPlaceholder(ElemKind::FloatTy, {NumDim, NumDim}, "A", false);
    B = Mod.createPlaceholder(ElemKind::FloatTy, {NumDim, NumDim}, "B", false);

    Exp = Mod.createPlaceholder(ElemKind::FloatTy, {NumDim, NumDim}, "exp",
                                false);
    auto HandleB = Bindings.allocate(B)->getHandle<float>();
    HandleB.randomize(-1, 1, Mod.getPRNG());
    Node *MM = F->createMatMul("matmul", A, B);
    auto *Reg = F->createRegression("reg", MM, Exp);
    Result = F->createSave("save", Reg);
  }

  Tensor Inputs(ElemKind::FloatTy, {{NumDim, NumDim}});
  Tensor Outputs(ElemKind::FloatTy, {{NumDim, NumDim}});

  auto InputsH = Inputs.getHandle<>();
  auto OutputsH = Outputs.getHandle<>();
  auto &Mod = EET_.getModule();

  InputsH.randomize(-1, 1, Mod.getPRNG());
  OutputsH.randomize(-1, 1, Mod.getPRNG());

  performGradCheck(EET_, EEI_, Bindings, Result->getPlaceholder(), A, Exp,
                   &Inputs, &Outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckBatchedReduceAddAxis0) {
  PlaceholderBindings Bindings;
  Placeholder *A, *Exp;
  SaveNode *Result;
  size_t BatchSize = 4;
  size_t NumRows = 3;
  size_t NumCols = 5;
  for (auto *EE : engines_) {
    auto &Mod = EE->getModule();
    Function *F = Mod.createFunction("main");

    A = Mod.createPlaceholder(ElemKind::FloatTy, {BatchSize, NumRows, NumCols},
                              "A", false);

    Exp = Mod.createPlaceholder(ElemKind::FloatTy, {NumRows, NumCols}, "exp",
                                false);

    TypeRef Ty = Mod.uniqueType(ElemKind::FloatTy, {NumRows, NumCols});
    Node *BRA = F->createBatchedReduceAdd("BRA", Ty, A, 0 /*axis*/);
    auto *Reg = F->createRegression("reg", BRA, Exp);
    Result = F->createSave("save", Reg);
  }

  Tensor Inputs(ElemKind::FloatTy, {{BatchSize, NumRows, NumCols}});
  Tensor Outputs(ElemKind::FloatTy, {{NumRows, NumCols}});

  auto InputsH = Inputs.getHandle<>();
  auto OutputsH = Outputs.getHandle<>();
  auto &Mod = EET_.getModule();
  InputsH.randomize(-1, 1, Mod.getPRNG());
  OutputsH.randomize(-1, 1, Mod.getPRNG());

  performGradCheck(EET_, EEI_, Bindings, Result->getPlaceholder(), A, Exp,
                   &Inputs, &Outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckBatchedReduceAddAxis1) {
  PlaceholderBindings Bindings;
  size_t NumRows = 3;
  size_t BatchSize = 4;
  size_t NumCols = 5;
  Placeholder *A, *Exp;
  SaveNode *Result;
  for (auto *EE : engines_) {
    auto &Mod = EE->getModule();
    Function *F = Mod.createFunction("main");

    A = Mod.createPlaceholder(ElemKind::FloatTy, {NumRows, BatchSize, NumCols},
                              "A", false);

    Exp = Mod.createPlaceholder(ElemKind::FloatTy, {NumRows, NumCols}, "exp",
                                false);

    TypeRef Ty = Mod.uniqueType(ElemKind::FloatTy, {NumRows, NumCols});
    Node *BRA = F->createBatchedReduceAdd("BRA", Ty, A, 1 /*axis*/);
    auto *Reg = F->createRegression("reg", BRA, Exp);
    Result = F->createSave("save", Reg);
  }

  Tensor Inputs(ElemKind::FloatTy, {{NumRows, BatchSize, NumCols}});
  Tensor Outputs(ElemKind::FloatTy, {{NumRows, NumCols}});

  auto InputsH = Inputs.getHandle<>();
  auto OutputsH = Outputs.getHandle<>();
  auto &Mod = EET_.getModule();
  InputsH.randomize(-1, 1, Mod.getPRNG());
  OutputsH.randomize(-1, 1, Mod.getPRNG());

  performGradCheck(EET_, EEI_, Bindings, Result->getPlaceholder(), A, Exp,
                   &Inputs, &Outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckGatherVec) {
  PlaceholderBindings Bindings;
  Placeholder *A, *Exp;
  SaveNode *Result;
  for (auto *EE : engines_) {
    auto &Mod = EE->getModule();
    Bindings.clear();
    Function *F = Mod.createFunction("main");

    A = Mod.createPlaceholder(ElemKind::FloatTy, {3, 4}, "A", false);
    auto *Indices = Mod.createPlaceholder(ElemKind::Int64ITy, {2}, "I", false);
    Bindings.allocate(Indices)->getHandle<int64_t>() = {0, 2};
    Exp = Mod.createPlaceholder(ElemKind::FloatTy, {2, 4}, "exp", false);

    Node *G = F->createGather("gather", A, Indices, 0 /*batchDims*/);
    auto *Reg = F->createRegression("reg", G, Exp);
    Result = F->createSave("save", Reg);
  }

  Tensor Inputs(ElemKind::FloatTy, {{3, 4}});
  Tensor Outputs(ElemKind::FloatTy, {{2, 4}});

  auto InputsH = Inputs.getHandle<>();
  auto OutputsH = Outputs.getHandle<>();
  auto &Mod = EET_.getModule();
  InputsH.randomize(-1, 1, Mod.getPRNG());
  OutputsH.randomize(-1, 1, Mod.getPRNG());

  performGradCheck(EET_, EEI_, Bindings, Result->getPlaceholder(), A, Exp,
                   &Inputs, &Outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckGatherDim) {
  PlaceholderBindings Bindings;
  Placeholder *A, *Exp;
  SaveNode *Result;
  for (auto *EE : engines_) {
    auto &Mod = EE->getModule();
    Bindings.clear();
    Function *F = Mod.createFunction("main");

    A = Mod.createPlaceholder(ElemKind::FloatTy, {8, 4}, "A", false);
    auto *Indices =
        Mod.createPlaceholder(ElemKind::Int64ITy, {2, 2}, "I", false);
    Bindings.allocate(Indices)->getHandle<int64_t>() = {0, 2, 3, 1};
    Exp = Mod.createPlaceholder(ElemKind::FloatTy, {2, 2, 4}, "exp", false);

    Node *G = F->createGather("gather", A, Indices, 0 /*batchDims*/);
    auto *Reg = F->createRegression("reg", G, Exp);
    Result = F->createSave("save", Reg);
  }

  Tensor Inputs(ElemKind::FloatTy, {{8, 4}});
  Tensor Outputs(ElemKind::FloatTy, {{2, 2, 4}});

  auto InputsH = Inputs.getHandle<>();
  auto OutputsH = Outputs.getHandle<>();
  auto &Mod = EET_.getModule();
  InputsH.randomize(-1, 1, Mod.getPRNG());
  OutputsH.randomize(-1, 1, Mod.getPRNG());

  performGradCheck(EET_, EEI_, Bindings, Result->getPlaceholder(), A, Exp,
                   &Inputs, &Outputs, 0.001, 0.01);
}

static void gradientCheckGroupConv(size_t depth, size_t group,
                                   ExecutionEngine2 &EET_,
                                   ExecutionEngine2 &EEI_) {
  PlaceholderBindings bindings;
  size_t numDim = 10;
  std::vector<ExecutionEngine2 *> engines;
  engines.push_back(&EEI_);
  engines.push_back(&EET_);
  Placeholder *A, *Ex;
  SaveNode *result;
  for (auto *EE : engines) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, depth},
                              "A", false);
    Ex = mod.createPlaceholder(
        ElemKind::FloatTy, {1, numDim + 1, numDim + 1, depth}, "exp", false);

    Node *O = F->createConv(bindings, "conv", A, depth, 2, 1, 1, group);
    O = F->createRegression("reg", O, Ex);
    result = F->createSave("ret", O);
  }

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, depth});
  Tensor outputs(ElemKind::FloatTy, {1, numDim + 1, numDim + 1, depth});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Ex,
                   &inputs, &outputs, 0.001, 0.04);
}

TEST_P(GradCheck, gradientCheckConv) {
  gradientCheckGroupConv(1, 1, EET_, EEI_);
}

TEST_P(GradCheck, gradientCheckDepthwiseConv) {
  gradientCheckGroupConv(4, 4, EET_, EEI_);
}

TEST_P(GradCheck, gradientCheckGroupConv) {
  gradientCheckGroupConv(4, 2, EET_, EEI_);
}

static void gradientCheckDilatedConv(size_t depth, size_t group,
                                     size_t dilation, ExecutionEngine2 &EET_,
                                     ExecutionEngine2 &EEI_) {
  PlaceholderBindings bindings;
  size_t numDim = 10;
  std::vector<ExecutionEngine2 *> engines;
  engines.push_back(&EEI_);
  engines.push_back(&EET_);
  Placeholder *A, *Ex;
  SaveNode *result;
  for (auto *EE : engines) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, depth},
                              "A", false);
    Ex = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, depth},
                               "exp", false);

    Node *O =
        F->createConv(bindings, "conv", A, depth, 2, 1, 1, group, dilation);
    O = F->createRegression("reg", O, Ex);
    result = F->createSave("ret", O);
  }

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, depth});
  Tensor outputs(ElemKind::FloatTy, {1, numDim, numDim, depth});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Ex,
                   &inputs, &outputs, 0.001, 0.04);
}

TEST_P(GradCheck, gradientCheckDilatedConv) {
  gradientCheckDilatedConv(1, 1, 2, EET_, EEI_);
}

TEST_P(InterpreterGrad, gradientCheckAvgPool) {
  PlaceholderBindings bindings;
  size_t numDim = 10;
  size_t numOutputElem = 10;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, 1}, "A",
                              false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "Exp",
                                false);

    Node *O = F->createAvgPool("pool", A, 3, 3, 1);
    O = F->createTanh("tanh", O);
    O = F->createFullyConnected(bindings, "fc", O, numOutputElem);
    O = F->createRegression("reg", O, Exp);
    result = F->createSave("ret", O);
  }

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, 1});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.001, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckBatchNorm) {
  PlaceholderBindings bindings;
  size_t numDim = 5;
  size_t numOutputElem = numDim * numDim * 3;
  Placeholder *A, *Ex;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, 3}, "A",
                              false);
    Ex = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "exp",
                               false);

    Node *O = F->createBatchNormalization(bindings, "batch", A, 3, 0.0001, 0.9);
    O = F->createReshape("reshape", O, {1, numDim * numDim * 3});
    O = F->createRegression("reg", O, Ex);
    result = F->createSave("ret", O);
  }

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, 3});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  for (auto &elem : inputsH) {
    elem = elem * 6 + 4;
  }

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Ex,
                   &inputs, &outputs, 0.001, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckArithmeticDiv) {
  // The test creates a net: A / B = Exp. Where A is trainable weight,
  // B and Exp are external data (initialized randomly once). SGD will find
  // correct value for A, and then gradient check will be performed.
  PlaceholderBindings bindings;
  size_t numDim = 10;
  Placeholder *B, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "A", true);
    B = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "B", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "exp", false);

    auto *ATensor = bindings.allocate(A);
    ATensor->init(Tensor::InitKind::Xavier, 1.0, mod.getPRNG());

    Node *O = F->createDiv("div", A, B);
    O = F->createRegression("reg", O, Exp);
    result = F->createSave("ret", O);
  }

  Tensor BValues(ElemKind::FloatTy, {1, numDim});
  Tensor ExpValues(ElemKind::FloatTy, {1, numDim});
  // Random values are in the range, so that all intermediate computations are
  // not too small and not too large.
  auto &mod = EET_.getModule();
  BValues.getHandle().randomize(0.1, 1, mod.getPRNG());
  ExpValues.getHandle().randomize(0.1, 1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), B, Exp,
                   &BValues, &ExpValues, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckArithmetic) {
  PlaceholderBindings bindings;
  size_t numDim = 20;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "A", false);
    auto *B = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "B", true);
    auto *C = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "C", false);
    auto *D = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "D", false);
    auto *E = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "E", false);
    bindings.allocate(B)->zero();
    bindings.allocate(C)->zero();
    bindings.allocate(D)->zero();

    // Randomize E to avoid div by zero.
    auto *ETensor = bindings.allocate(E);
    ETensor->getHandle().randomize(-1, 1, mod.getPRNG());

    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "exp", false);

    Node *O = F->createMul("mul", A, B);
    O = F->createAdd("add", O, C);
    O = F->createSub("sub", D, O);
    O = F->createDiv("div", O, E);
    O = F->createRegression("reg", O, Exp);
    result = F->createSave("ret", O);
  }

  Tensor inputs(ElemKind::FloatTy, {1, numDim});
  Tensor outputs(ElemKind::FloatTy, {1, numDim});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.01, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckFCConcatTanh) {
  // Using the same gradient check test setup as gradientCheck_FC_Concat_RELU
  PlaceholderBindings bindings;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "Exp",
                                false);

    Node *FA = F->createFullyConnected(bindings, "fc", A, numOutputElem);
    FA = F->createTanh("tanh", FA);
    FA = F->createRegression("reg", FA, Exp);
    result = F->createSave("ret", FA);
  }
  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckFC) {
  PlaceholderBindings bindings;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "Exp",
                                false);

    Node *FA = F->createFullyConnected(bindings, "fc", A, numOutputElem);
    FA = F->createRegression("reg", FA, Exp);
    result = F->createSave("ret", FA);
  }

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.0001, 0.0001);
}

TEST_P(InterpreterGrad, gradientCheckSigmoid) {
  PlaceholderBindings bindings;
  size_t numInputElem = 20;
  size_t numOutputElem = 20;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "Exp",
                                false);

    Node *FA = F->createSigmoid("sig", A);
    FA = F->createRegression("reg", FA, Exp);
    result = F->createSave("ret", FA);
  }

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckRelu) {
  PlaceholderBindings bindings;
  size_t numInputElem = 20;
  size_t numOutputElem = 20;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "Exp",
                                false);

    Node *FA = F->createRELU("relu", A);
    FA = F->createRegression("reg", FA, Exp);
    result = F->createSave("ret", FA);
  }

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.001, 0.01);
}

TEST_P(InterpreterGrad, gradientCheckTranspose) {
  // Using the same gradient check test setup as gradientCheck_FC_Concat_RELU
  PlaceholderBindings bindings;
  size_t numOutputElem = 10;
  Placeholder *A, *Exp;
  SaveNode *result;
  for (auto *EE : engines_) {
    auto &mod = EE->getModule();
    bindings.clear();
    Function *F = mod.createFunction("main");
    A = mod.createPlaceholder(ElemKind::FloatTy, {1, 5, 10, 5}, "input", false);
    Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "exp",
                                false);
    Node *TA = F->createTranspose("transpose", A, NHWC2NCHW);
    TA = F->createFullyConnected(bindings, "fc", TA, numOutputElem);
    TA = F->createRegression("regress", TA, Exp);
    result = F->createSave("ret", TA);
  }

  Tensor inputs(ElemKind::FloatTy, {1, 5, 10, 5});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();
  auto &mod = EET_.getModule();
  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EET_, EEI_, bindings, result->getPlaceholder(), A, Exp,
                   &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckCrossEntropyLoss) {
  const size_t batchSize = 6;
  const int testSamples = 5;
  const float stepSize = 1e-4;
  const float delta = 0.015;
  TrainingConfig TC;
  PlaceholderBindings bindings;

  auto &mod = EET_.getModule();
  Function *F = mod.createFunction("main");
  auto *P =
      mod.createPlaceholder(ElemKind::FloatTy, {batchSize, 4}, "P", false);
  bindings.allocate(P)->zero();
  auto *Y =
      mod.createPlaceholder(ElemKind::Int64ITy, {batchSize}, "Labels", false);
  bindings.allocate(Y)->zero();
  Node *CE = F->createCrossEntropyLoss("celoss", P, Y);
  auto *result = F->createSave("ret", CE);
  auto *LTensor = bindings.allocate(result->getPlaceholder());

  Tensor inputs(ElemKind::FloatTy, {batchSize, 4});
  inputs.zero();
  Tensor outputs(ElemKind::Int64ITy, {batchSize});
  outputs.zero();

  auto inputsH = inputs.getHandle();
  auto outputsH = outputs.getHandle<int64_t>();

  inputsH.randomize(0.0, 1.0, mod.getPRNG());
  outputsH.at({0}) = 2;
  outputsH.at({1}) = 0;
  outputsH.at({2}) = 1;

  VariableGradientsList varGrads;
  glow::differentiate(F, TC, "record", &varGrads);
  allocateGrads(bindings, varGrads);
  EET_.compile(CompilationMode::Train);

  auto *gradPlaceholder = getGrad(varGrads, P);
  auto gradTensorHandle = bindings.get(gradPlaceholder)->getHandle();

  for (int i = 0; i < testSamples; ++i) {
    inputsH.randomize(0.0, 1.0, mod.getPRNG());
    for (size_t j = 0; j < inputsH.size(); ++j) {
      updateInputPlaceholders2(bindings, {P, Y}, {&inputs, &outputs});
      EET_.run(bindings, "record");
      LTensor->zero();
      auto x = inputsH.raw(j);
      auto g = gradTensorHandle.raw(j);
      inputsH.raw(j) = x + stepSize;
      updateInputPlaceholders2(bindings, {P, Y}, {&inputs, &outputs});
      EET_.run(bindings, "record");
      auto lp = LTensor->getHandle().raw(0);
      inputsH.raw(j) = x - stepSize;
      LTensor->zero();
      updateInputPlaceholders2(bindings, {P, Y}, {&inputs, &outputs});
      EET_.run(bindings, "record");
      auto lm = LTensor->getHandle().raw(0);
      auto diff = (lp - lm) / (2 * stepSize);
      inputsH.raw(j) = x;
      updateInputPlaceholders2(bindings, {P, Y}, {&inputs, &outputs});
      EET_.run(bindings, "record");
      EXPECT_NEAR(diff, g, delta);
    }
  }
}

INSTANTIATE_TEST_CASE_P(Interpreter, InterpreterGrad,
                        ::testing::Values("Interpreter"));

INSTANTIATE_TEST_CASE_P(Interpreter, GradCheck,
                        ::testing::Values("Interpreter"));

#ifdef GLOW_WITH_CPU
INSTANTIATE_TEST_CASE_P(JIT, GradCheck, ::testing::Values("CPU"));
#endif // GLOW_WITH_CPU
