//
// Copyright (c) 2012, University of Erlangen-Nuremberg
// Copyright (c) 2012, Siemens AG
// Copyright (c) 2010, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

//===--- MemoryAccess.cpp - Rewrite Memory Accesses in the AST ------------===//
//
// This file implements the translation of memory accesses to different memory
// regions.
//
//===----------------------------------------------------------------------===//

#include "hipacc/AST/ASTTranslate.h"

using namespace clang;
using namespace hipacc;
using namespace ASTNode;
using namespace hipacc::Builtin;


// add local offset to index
Expr *ASTTranslate::addLocalOffset(Expr *idx, Expr *local_offset) {
  if (local_offset) {
    idx = createBinaryOperator(Ctx, idx, Clone(local_offset), BO_Add,
        Ctx.IntTy);
  }

  return idx;
}


// add global offset to index
Expr *ASTTranslate::addGlobalOffsetY(Expr *idx_y, HipaccAccessor *Acc) {
  if (Acc->getOffsetYDecl()) {
    idx_y = createBinaryOperator(Ctx, idx_y, getOffsetYDecl(Acc), BO_Add,
        Ctx.IntTy);
  }

  return idx_y;
}
Expr *ASTTranslate::addGlobalOffsetX(Expr *idx_x, HipaccAccessor *Acc) {
  if (Acc->getOffsetXDecl()) {
    idx_x = createBinaryOperator(Ctx, idx_x, getOffsetXDecl(Acc), BO_Add,
        Ctx.IntTy);
  }

  return idx_x;
}


// remove iteration space offset from index
Expr *ASTTranslate::removeISOffsetX(Expr *idx_x, HipaccAccessor *Acc) {
  if (Kernel->getIterationSpace()->getAccessor()->getOffsetXDecl()) {
      idx_x = createBinaryOperator(Ctx, idx_x,
          getOffsetXDecl(Kernel->getIterationSpace()->getAccessor()), BO_Sub,
          Ctx.IntTy);
  }

  return idx_x;
}


// remove iteration space offset from index
Expr *ASTTranslate::removeISOffsetY(Expr *idx_y, HipaccAccessor *Acc) {
  if (Kernel->getIterationSpace()->getAccessor()->getOffsetYDecl()) {
      idx_y = createBinaryOperator(Ctx, idx_y,
          getOffsetYDecl(Kernel->getIterationSpace()->getAccessor()), BO_Sub,
          Ctx.IntTy);
  }

  return idx_y;
}


// access memory
Expr *ASTTranslate::accessMem(DeclRefExpr *LHS, HipaccAccessor *Acc,
    MemoryAccess memAcc, Expr *local_offset_x, Expr *local_offset_y) {
  Expr *idx_x = tileVars.global_id_x;
  Expr *idx_y = gidYRef;

  // step 0: add local offset: gid_[x|y] + local_offset_[x|y]
  idx_x = addLocalOffset(idx_x, local_offset_x);
  idx_y = addLocalOffset(idx_y, local_offset_y);

  // step 1: remove is_offset and add interpolation & boundary handling
  switch (Acc->getInterpolation()) {
    case InterpolateNO:
      if (Acc!=Kernel->getIterationSpace()->getAccessor()) {
        idx_x = removeISOffsetX(idx_x, Acc);
      }
      if ((compilerOptions.emitC() ||
           compilerOptions.emitRenderscript() ||
           compilerOptions.emitFilterscript()) &&
          Acc!=Kernel->getIterationSpace()->getAccessor()) {
        idx_y = removeISOffsetY(idx_y, Acc);
      }
      break;
    case InterpolateNN:
      idx_x = createCStyleCastExpr(Ctx, Ctx.IntTy, CK_FloatingToIntegral,
          createParenExpr(Ctx, addNNInterpolationX(Acc, idx_x)), nullptr,
          Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
      idx_y = createCStyleCastExpr(Ctx, Ctx.IntTy, CK_FloatingToIntegral,
          createParenExpr(Ctx, addNNInterpolationY(Acc, idx_y)), nullptr,
          Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
      break;
    case InterpolateLF:
    case InterpolateCF:
    case InterpolateL3:
      return addInterpolationCall(LHS, Acc, idx_x, idx_y);
  }

  // step 2: add global Accessor/Iteration Space offset
  if (Acc!=Kernel->getIterationSpace()->getAccessor()) {
    idx_x = addGlobalOffsetX(idx_x, Acc);
    idx_y = addGlobalOffsetY(idx_y, Acc);
  } else {
    if (!(compilerOptions.emitC() ||
          compilerOptions.emitRenderscript() ||
          compilerOptions.emitFilterscript())) {
      idx_y = addGlobalOffsetY(idx_y, Acc);
    }
  }

  // step 3: access the appropriate memory
  switch (memAcc) {
    case WRITE_ONLY:
      switch (compilerOptions.getTargetCode()) {
        default: break;
        case TARGET_Renderscript: {
            bool isGlobalAllocation = false;
            for (size_t i=0; i<Kernel->getKernelClass()->getNumArgs(); ++i) {
              if (Kernel->getKernelClass()->getArguments()[0].name.compare(
                    LHS->getNameInfo().getAsString()) == 0) {
                isGlobalAllocation = true;
                break;
              }
            }
            if (!isGlobalAllocation) {
              // access allocation by using local pointer type kernel argument
              return accessMemAllocPtr(LHS);
            }
          }
          break;
        case TARGET_Filterscript:
          assert(0 && "Filterscript does not support write access for allocations.");
      }
    case READ_ONLY:
      switch (compilerOptions.getTargetCode()) {
        case TARGET_CUDA:
          if (Kernel->useTextureMemory(Acc)) {
            return accessMemTexAt(LHS, Acc, memAcc, idx_x, idx_y);
          }
          // fall through
        case TARGET_OpenCLACC:
        case TARGET_OpenCLCPU:
        case TARGET_OpenCLGPU:
          if (Kernel->useTextureMemory(Acc)) {
            return accessMemImgAt(LHS, Acc, memAcc, idx_x, idx_y);
          }
          return accessMemArrAt(LHS, getStrideDecl(Acc), idx_x, idx_y);
        case TARGET_C:
          return accessMem2DAt(LHS, idx_x, idx_y);
        case TARGET_Renderscript:
        case TARGET_Filterscript:
          return accessMemAllocAt(LHS, memAcc, idx_x, idx_y);
      }
    case READ_WRITE: {
      unsigned int DiagIDRW = Diags.getCustomDiagID(DiagnosticsEngine::Error,
          "Reading and writing to Image '%0' in kernel '%1' is not supported.");
      Diags.Report(DiagIDRW) << LHS->getNameInfo().getAsString()
                             << KernelClass->getName();
      exit(EXIT_FAILURE); }
    default:
    case UNDEFINED: {
      unsigned int DiagIDU = Diags.getCustomDiagID(DiagnosticsEngine::Error,
          "Memory access pattern for Image '%0' in kernel '%1' could not be analyzed.");
      Diags.Report(DiagIDU) << LHS->getNameInfo().getAsString()
                            << KernelClass->getName();
      exit(EXIT_FAILURE); }
  }
}


// access 1D memory array at given index
Expr *ASTTranslate::accessMemArrAt(DeclRefExpr *LHS, Expr *stride, Expr *idx_x,
    Expr *idx_y) {
  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  // for vectorization divide stride by vector size
  if (Kernel->vectorize()) {
    stride = createBinaryOperator(Ctx, stride, createIntegerLiteral(Ctx, 4),
        BO_Div, Ctx.IntTy);
  }

  Expr *result = createBinaryOperator(Ctx, createBinaryOperator(Ctx,
        createParenExpr(Ctx, idx_y), stride, BO_Mul, Ctx.IntTy), idx_x, BO_Add,
      Ctx.IntTy);

  result = new (Ctx) ArraySubscriptExpr(LHS, result,
      LHS->getType()->getPointeeType(), VK_LValue, OK_Ordinary,
      SourceLocation());

  return result;
}


// access 2D memory array at given index
Expr *ASTTranslate::accessMem2DAt(DeclRefExpr *LHS, Expr *idx_x, Expr *idx_y) {
  QualType QT = LHS->getType();
  QualType QT2 = QT->getPointeeType()->getAsArrayTypeUnsafe()->getElementType();

  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  Expr *result = new (Ctx) ArraySubscriptExpr(createImplicitCastExpr(Ctx, QT,
        CK_LValueToRValue, LHS, nullptr, VK_RValue), idx_y,
        QT->getPointeeType(), VK_LValue, OK_Ordinary, SourceLocation());

  result = new (Ctx) ArraySubscriptExpr(createImplicitCastExpr(Ctx,
        Ctx.getPointerType(QT2), CK_ArrayToPointerDecay, result, nullptr,
        VK_RValue), idx_x, QT2, VK_LValue, OK_Ordinary, SourceLocation());

  return result;
}


// get tex1Dfetch function for given Accessor
FunctionDecl *ASTTranslate::getTextureFunction(HipaccAccessor *Acc, MemoryAccess
    memAcc) {
  QualType QT = Acc->getImage()->getType();
  bool isVecType = QT->isVectorType();

  if (isVecType) {
    QT = QT->getAs<VectorType>()->getElementType();
  }
  const BuiltinType *BT = QT->getAs<BuiltinType>();

  bool isOneDim = false, isLdg = false;
  switch (Kernel->useTextureMemory(Acc)) {
    default:
      break;
    case Linear1D:
      isOneDim = true;
      break;
    case Ldg:
      isLdg = true;
      break;
  }

  switch (BT->getKind()) {
    case BuiltinType::WChar_U:
    case BuiltinType::WChar_S:
    case BuiltinType::ULongLong:
    case BuiltinType::UInt128:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
    case BuiltinType::LongDouble:
    case BuiltinType::Void:
    case BuiltinType::Bool:
    case BuiltinType::Long:
    case BuiltinType::ULong:
    case BuiltinType::Double:
    default:
      assert(0 && "BuiltinType for CUDA texture not supported.");

#define GET_BUILTIN_FUNCTION(TYPE) \
      (memAcc == READ_ONLY ? \
          (isLdg ? \
              (isVecType ? builtins.getBuiltinFunction(CUDABI__ldg ## E4 ## TYPE) : \
                  builtins.getBuiltinFunction(CUDABI__ldg ## TYPE)) : \
              (isOneDim ? \
                  (isVecType ? builtins.getBuiltinFunction(CUDABItex1Dfetch ## E4 ## TYPE) : \
                      builtins.getBuiltinFunction(CUDABItex1Dfetch ## TYPE)) : \
                  (isVecType ? builtins.getBuiltinFunction(CUDABItex2D ## E4 ## TYPE) : \
                      builtins.getBuiltinFunction(CUDABItex2D ## TYPE)))) : \
          (isVecType ? builtins.getBuiltinFunction(CUDABIsurf2Dwrite ## E4 ## TYPE) : \
              builtins.getBuiltinFunction(CUDABIsurf2Dwrite ## TYPE)))

    case BuiltinType::Char_S:
    case BuiltinType::SChar:
      return GET_BUILTIN_FUNCTION(Sc);
    case BuiltinType::Char_U:
    case BuiltinType::UChar:
      return GET_BUILTIN_FUNCTION(Uc);
    case BuiltinType::Short:
      return GET_BUILTIN_FUNCTION(s);
    case BuiltinType::Char16:
    case BuiltinType::UShort:
      return GET_BUILTIN_FUNCTION(Us);
    case BuiltinType::Int:
      return GET_BUILTIN_FUNCTION(i);
    case BuiltinType::Char32:
    case BuiltinType::UInt:
      return GET_BUILTIN_FUNCTION(Ui);
    case BuiltinType::Float:
      return GET_BUILTIN_FUNCTION(f);
#undef GET_BUILTIN_FUNCTION
  }
}


// get read_image function for given Accessor
FunctionDecl *ASTTranslate::getImageFunction(HipaccAccessor *Acc, MemoryAccess
    memAcc) {
  QualType QT = Acc->getImage()->getType();

  if (QT->isVectorType()) {
      QT = QT->getAs<VectorType>()->getElementType();
  }
  const BuiltinType *BT = QT->getAs<BuiltinType>();

  switch (BT->getKind()) {
    case BuiltinType::WChar_U:
    case BuiltinType::WChar_S:
    case BuiltinType::ULongLong:
    case BuiltinType::UInt128:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
    case BuiltinType::LongDouble:
    case BuiltinType::Void:
    case BuiltinType::Bool:
    case BuiltinType::Long:
    case BuiltinType::ULong:
    case BuiltinType::Double:
    default:
      assert(0 && "BuiltinType for OpenCL Image not supported.");
    case BuiltinType::Char_S:
    case BuiltinType::SChar:
    case BuiltinType::Short:
    case BuiltinType::Int:
      if (memAcc==READ_ONLY) {
        return builtins.getBuiltinFunction(OPENCLBIread_imagei);
      } else {
        return builtins.getBuiltinFunction(OPENCLBIwrite_imagei);
      }
    case BuiltinType::Char_U:
    case BuiltinType::UChar:
    case BuiltinType::Char16:
    case BuiltinType::UShort:
    case BuiltinType::Char32:
    case BuiltinType::UInt:
      if (memAcc==READ_ONLY) {
        return builtins.getBuiltinFunction(OPENCLBIread_imageui);
      } else {
        return builtins.getBuiltinFunction(OPENCLBIwrite_imageui);
      }
    case BuiltinType::Float:
      if (memAcc==READ_ONLY) {
        return builtins.getBuiltinFunction(OPENCLBIread_imagef);
      } else {
        return builtins.getBuiltinFunction(OPENCLBIwrite_imagef);
      }
  }
}


// get rsGetElementAt_<type>/rsSetElementAt_<type> functions for given Accessor
FunctionDecl *ASTTranslate::getAllocationFunction(const BuiltinType *BT, bool
    isVecType, MemoryAccess memAcc) {
  switch (BT->getKind()) {
    case BuiltinType::WChar_U:
    case BuiltinType::WChar_S:
    case BuiltinType::ULongLong:
    case BuiltinType::UInt128:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
    case BuiltinType::LongDouble:
    case BuiltinType::Void:
    case BuiltinType::Bool:
    default:
      assert(0 && "BuiltinType for Renderscript Allocation not supported.");

#define GET_BUILTIN_FUNCTION(TYPE) \
    (memAcc == READ_ONLY ? \
        (isVecType ? builtins.getBuiltinFunction(RSBIrsGetElementAt_ ## TYPE ## 4) : \
            builtins.getBuiltinFunction(RSBIrsGetElementAt_ ## TYPE)) : \
        (isVecType ? builtins.getBuiltinFunction(RSBIrsSetElementAt_ ## TYPE ## 4) : \
            builtins.getBuiltinFunction(RSBIrsSetElementAt_ ## TYPE)))

    case BuiltinType::Char_S:
    case BuiltinType::SChar:
      return GET_BUILTIN_FUNCTION(char);
    case BuiltinType::Short:
      return GET_BUILTIN_FUNCTION(short);
    case BuiltinType::Int:
      return GET_BUILTIN_FUNCTION(int);
    case BuiltinType::Long:
      return GET_BUILTIN_FUNCTION(long);
    case BuiltinType::Char_U:
    case BuiltinType::UChar:
      return GET_BUILTIN_FUNCTION(uchar);
    case BuiltinType::Char16:
    case BuiltinType::UShort:
      return GET_BUILTIN_FUNCTION(ushort);
    case BuiltinType::Char32:
    case BuiltinType::UInt:
      return GET_BUILTIN_FUNCTION(uint);
    case BuiltinType::ULong:
      return GET_BUILTIN_FUNCTION(ulong);
    case BuiltinType::Float:
      return GET_BUILTIN_FUNCTION(float);
    case BuiltinType::Double:
      return GET_BUILTIN_FUNCTION(double);
#undef GET_BUILTIN_FUNCTION
  }
}


// get convert_<type> function for given type
FunctionDecl *ASTTranslate::getConvertFunction(QualType QT, bool isVecType) {
  assert(isVecType && "Only vector types are supported yet.");
  if (isVecType) {
    QT = QT->getAs<VectorType>()->getElementType();
  }
  std::string name = "convert_";

  switch (QT->getAs<BuiltinType>()->getKind()) {
    case BuiltinType::WChar_U:
    case BuiltinType::WChar_S:
    case BuiltinType::ULongLong:
    case BuiltinType::UInt128:
    case BuiltinType::LongLong:
    case BuiltinType::Int128:
    case BuiltinType::LongDouble:
    case BuiltinType::Void:
    case BuiltinType::Bool:
    default:
      assert(0 && "BuiltinType for 'convert' function not supported.");
    case BuiltinType::Char_S:
    case BuiltinType::SChar:
      name += "char4";
      QT = Ctx.CharTy;
      break;
    case BuiltinType::Short:
      name += "short4";
      QT = Ctx.ShortTy;
      break;
    case BuiltinType::Int:
      name += "int4";
      QT = Ctx.IntTy;
      break;
    case BuiltinType::Long:
      name += "long4";
      QT = Ctx.LongTy;
      break;
    case BuiltinType::Char_U:
    case BuiltinType::UChar:
      name += "uchar4";
      QT = Ctx.UnsignedCharTy;
      break;
    case BuiltinType::Char16:
    case BuiltinType::UShort:
      name += "ushort4";
      QT = Ctx.UnsignedShortTy;
      break;
    case BuiltinType::Char32:
    case BuiltinType::UInt:
      name += "uint4";
      QT = Ctx.UnsignedIntTy;
      break;
    case BuiltinType::ULong:
      name += "ulong4";
      QT = Ctx.UnsignedLongTy;
      break;
    case BuiltinType::Float:
      name += "float4";
      QT = Ctx.FloatTy;
      break;
    case BuiltinType::Double:
      name += "double4";
      QT = Ctx.DoubleTy;
      break;
  }

  FunctionDecl *result = lookup<FunctionDecl>(name, simdTypes.getSIMDType(QT,
        QT.getAsString(), SIMD4), hipaccNS);
  assert(result && "could not lookup convert function");

  return result;
}


// access linear texture memory at given index
Expr *ASTTranslate::accessMemTexAt(DeclRefExpr *LHS, HipaccAccessor *Acc,
    MemoryAccess memAcc, Expr *idx_x, Expr *idx_y) {
  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  FunctionDecl *texture_function = getTextureFunction(Acc, memAcc);

  // clone Decl
  TemplateArgumentListInfo templateArgs(LHS->getLAngleLoc(),
      LHS->getRAngleLoc());
  for (size_t i=0, e=LHS->getNumTemplateArgs(); i!=e; ++i) {
    templateArgs.addArgument(LHS->getTemplateArgs()[i]);
  }

  assert(isa<ParmVarDecl>(LHS->getDecl()) && "texture variable must be a ParmVarDecl!");
  ParmVarDecl *PVD = dyn_cast<ParmVarDecl>(LHS->getDecl());
  DeclRefExpr *LHStex = DeclRefExpr::Create(Ctx,
      LHS->getQualifierLoc(),
      LHS->getTemplateKeywordLoc(),
      CloneDeclTex(PVD, (memAcc==READ_ONLY)?"_tex":"_surf"),
      LHS->refersToEnclosingLocal(),
      LHS->getLocation(),
      LHS->getType(), LHS->getValueKind(),
      LHS->getFoundDecl(),
      LHS->getNumTemplateArgs()?&templateArgs:0);

  setExprProps(LHS, LHStex);

  // parameters for __ldg, tex1Dfetch, tex2D, or surf2Dwrite
  SmallVector<Expr *, 16> args;

  if (memAcc == READ_ONLY) {
    switch (Kernel->useTextureMemory(Acc)) {
      default:
      case Linear1D:
        args.push_back(LHStex);
        args.push_back(createBinaryOperator(Ctx, createBinaryOperator(Ctx,
                createParenExpr(Ctx, idx_y), getStrideDecl(Acc), BO_Mul,
                Ctx.IntTy), idx_x, BO_Add, Ctx.IntTy));
        break;
      case Linear2D:
      case Array2D:
        args.push_back(LHStex);
        args.push_back(idx_x);
        args.push_back(idx_y);
        break;
      case Ldg:
        // __ldg(&arr[idx])
        args.push_back(createUnaryOperator(Ctx, accessMemArrAt(LHS,
                getStrideDecl(Acc), idx_x, idx_y), UO_AddrOf, Ctx.IntTy));
        break;
    }
  } else {
    // writeImageRHS is set by VisitBinaryOperator - side effect
    writeImageRHS = createParenExpr(Ctx, writeImageRHS);
    args.push_back(writeImageRHS);
    args.push_back(LHStex);
    // byte addressing required for surf2Dwrite
    args.push_back(createBinaryOperator(Ctx, idx_x, createIntegerLiteral(Ctx,
            (int32_t)Acc->getImage()->getPixelSize()), BO_Mul, Ctx.IntTy));
    args.push_back(idx_y);
  }

  return createFunctionCall(Ctx, texture_function, args);
}


// access image memory at given index
Expr *ASTTranslate::accessMemImgAt(DeclRefExpr *LHS, HipaccAccessor *Acc,
    MemoryAccess memAcc, Expr *idx_x, Expr *idx_y) {
  Expr *result, *coord;

  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  // construct coordinate: (int2)(gid_x, gid_y)
  coord = createBinaryOperator(Ctx, idx_x, idx_y, BO_Comma, Ctx.IntTy);
  coord = createParenExpr(Ctx, coord);
  QualType QTcoord = simdTypes.getSIMDType(Ctx.IntTy, "int", SIMD2);
  coord = createCStyleCastExpr(Ctx, QTcoord, CK_VectorSplat, coord, nullptr,
      Ctx.getTrivialTypeSourceInfo(QTcoord));
  FunctionDecl *image_function = getImageFunction(Acc, memAcc);

  // create function call for image objects in OpenCL
  if (memAcc == READ_ONLY) {
    // parameters for read_image
    SmallVector<Expr *, 16> args;
    args.push_back(LHS);
    args.push_back(kernelSamplerRef);
    args.push_back(coord);

    result = createFunctionCall(Ctx, image_function, args);

    QualType QT = Acc->getImage()->getType();
    if (QT->isVectorType()) {
      SmallVector<Expr *, 16> args;
      args.push_back(result);
      result = createFunctionCall(Ctx, getConvertFunction(QT, true), args);
    } else {
      result = createExtVectorElementExpr(Ctx, QT, result, "x");
    }
  } else {
    QualType QT;

    // determine cast type for write_image functions
    if (image_function == builtins.getBuiltinFunction(OPENCLBIwrite_imagei)) {
      QT = simdTypes.getSIMDType(Ctx.IntTy, "int", SIMD4);
    } else if (image_function ==
        builtins.getBuiltinFunction(OPENCLBIwrite_imageui)) {
      QT = simdTypes.getSIMDType(Ctx.UnsignedIntTy, "uint", SIMD4);
    } else {
      QT = simdTypes.getSIMDType(Ctx.FloatTy, "float", SIMD4);
    }

    // writeImageRHS is set by VisitBinaryOperator - side effect
    if (!writeImageRHS->getType()->isVectorType()) {
      // introduce temporary for propagating the RHS to a vector
      std::stringstream LSST;
      LSST << "_tmp" << literalCount++;
      VarDecl *tmp_decl = createVarDecl(Ctx, kernelDecl, LSST.str(), QT,
          writeImageRHS);
      DeclContext *DC = FunctionDecl::castToDeclContext(kernelDecl);
      DC->addDecl(tmp_decl);
      DeclRefExpr *tmp_dre = createDeclRefExpr(Ctx, tmp_decl);
      preStmts.push_back(createDeclStmt(Ctx, tmp_decl));
      preCStmt.push_back(curCStmt);
      writeImageRHS = tmp_dre;
    }

    if (writeImageRHS->getType() != QT) {
      // convert to proper vector type
      SmallVector<Expr *, 16> args;
      args.push_back(writeImageRHS);
      writeImageRHS = createFunctionCall(Ctx, getConvertFunction(QT, true),
          args);
    }

    // parameters for write_image
    SmallVector<Expr *, 16> args;
    args.push_back(LHS);
    args.push_back(coord);
    args.push_back(writeImageRHS);

    result = createFunctionCall(Ctx, image_function, args);
  }

  return result;
}


// access allocation at given index
Expr *ASTTranslate::accessMemAllocAt(DeclRefExpr *LHS, MemoryAccess memAcc,
                                     Expr *idx_x, Expr *idx_y) {
  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  QualType QT = LHS->getType()->getPointeeType();
  bool isVec = QT->isVectorType();

  if (isVec) {
    QT = QT->getAs<VectorType>()->getElementType();
  }
  const BuiltinType *BT = QT->getAs<BuiltinType>();
  FunctionDecl *element_function = getAllocationFunction(BT, isVec, memAcc);

  //const BuiltinType *BT = LHS->getType()->getPointeeType()->getAs<BuiltinType>();
  //FunctionDecl *get_element_function = getAllocationFunction(BT, false, memAcc);

  // parameters for rsGetElementAt_<type>
  SmallVector<Expr *, 16> args;
  args.push_back(LHS);
  if (memAcc == WRITE_ONLY) {
    // writeImageRHS is set by VisitBinaryOperator - side effect
    writeImageRHS = createParenExpr(Ctx, writeImageRHS);
    args.push_back(writeImageRHS);
  }
  args.push_back(idx_x);
  args.push_back(idx_y);

  return createFunctionCall(Ctx, element_function, args);
}


// access allocation through kernel parameter
Expr *ASTTranslate::accessMemAllocPtr(DeclRefExpr *LHS) {
  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  return createUnaryOperator(Ctx, LHS, UO_Deref, LHS->getType());
}


// access shared memory
Expr *ASTTranslate::accessMemShared(DeclRefExpr *LHS, Expr *local_offset_x, Expr
    *local_offset_y) {
  Expr *idx_x = tileVars.local_id_x;
  Expr *idx_y = lidYRef;

  // step 0: add local offset: lid_[x|y] + local_offset_[x|y]
  idx_x = addLocalOffset(idx_x, local_offset_x);
  idx_y = addLocalOffset(idx_y, local_offset_y);

  return accessMemSharedAt(LHS, idx_x, idx_y);
}


// access shared memory at given index
Expr *ASTTranslate::accessMemSharedAt(DeclRefExpr *LHS, Expr *idx_x, Expr
    *idx_y) {
  QualType QT =
    LHS->getType()->castAsArrayTypeUnsafe()->getElementType()->castAsArrayTypeUnsafe()->getElementType();
  QualType QT2 = LHS->getType()->castAsArrayTypeUnsafe()->getElementType();

  // mark image as being used within the kernel
  Kernel->setUsed(LHS->getNameInfo().getAsString());

  // calculate index: [idx_y][idx_x]
  Expr *result = new (Ctx) ArraySubscriptExpr(createImplicitCastExpr(Ctx, QT2,
        CK_LValueToRValue, LHS, nullptr, VK_RValue), idx_y, QT2->getPointeeType(),
      VK_LValue, OK_Ordinary, SourceLocation());

  result = new (Ctx) ArraySubscriptExpr(createImplicitCastExpr(Ctx,
        Ctx.getPointerType(QT), CK_ArrayToPointerDecay, result, nullptr,
        VK_RValue), idx_x, QT, VK_LValue, OK_Ordinary, SourceLocation());

  return result;
}


// stage single image line (warp size) to shared memory
void ASTTranslate::stageLineToSharedMemory(ParmVarDecl *PVD,
    SmallVector<Stmt *, 16> &stageBody, Expr *local_offset_x, Expr
    *local_offset_y, Expr *global_offset_x, Expr *global_offset_y) {
  VarDecl *VD = KernelDeclMapShared[PVD];
  HipaccAccessor *Acc = KernelDeclMapAcc[PVD];
  DeclRefExpr *paramDRE = createDeclRefExpr(Ctx, PVD);

  Expr *LHS = accessMemShared(createDeclRefExpr(Ctx, VD), local_offset_x,
      local_offset_y);

  Expr *RHS;
  if (bh_variant.borderVal) {
    SmallVector<Stmt *, 16> bhStmts;
    SmallVector<CompoundStmt *, 16> bhCStmt;
    RHS = addBorderHandling(paramDRE, global_offset_x, global_offset_y, Acc,
        bhStmts, bhCStmt);

    // add border handling statements to stageBody
    for (size_t i=0, e=bhStmts.size(); i!=e; ++i) {
      stageBody.push_back(bhStmts.data()[i]);
    }
  } else {
    RHS = accessMem(paramDRE, Acc, READ_ONLY, global_offset_x, global_offset_y);
  }

  stageBody.push_back(createBinaryOperator(Ctx, LHS, RHS, BO_Assign,
        Acc->getImage()->getType()));
}


// stage iteration p to shared memory
void ASTTranslate::stageIterationToSharedMemory(SmallVector<Stmt *, 16>
    &stageBody, int p) {
  for (auto I=kernelDecl->param_begin(), N=kernelDecl->param_end(); I!=N; ++I) {
    ParmVarDecl *PVD = *I;

    if (KernelDeclMapShared[PVD]) {
      HipaccAccessor *Acc = KernelDeclMapAcc[PVD];

      // check if the bottom apron has to be fetched
      if (p>=(int)Kernel->getPixelsPerThread()) {
        int p_add = (int)ceilf((Acc->getSizeY()-1) /
            (float)Kernel->getNumThreadsY());
        if (p>=(int)Kernel->getPixelsPerThread()+p_add) continue;
      }

      Expr *global_offset_x = nullptr, *global_offset_y = nullptr;
      Expr *SX2;

      if (Acc->getSizeX() > 1) {
        if (compilerOptions.exploreConfig()) {
          SX2 = tileVars.local_size_x;
        } else {
          SX2 = createIntegerLiteral(Ctx, (int32_t)Kernel->getNumThreadsX());
        }
      } else {
        SX2 = createIntegerLiteral(Ctx, 0);
      }
      if (Acc->getSizeY() > 1) {
        global_offset_y = createParenExpr(Ctx, createUnaryOperator(Ctx,
              createIntegerLiteral(Ctx, (int32_t)Acc->getSizeY()/2), UO_Minus,
              Ctx.IntTy));
      } else {
        global_offset_y = nullptr;
      }


      // check if we need to stage right apron
      size_t num_stages_x = 0;
      if (Acc->getSizeX() > 1) {
          num_stages_x = 2;
      }

      // load row (line)
      for (size_t i=0; i<=num_stages_x; ++i) {
        // _smem[lidYRef][(int)threadIdx.x + i*(int)blockDim.x] =
        //        Image[-SX/2 + i*(int)blockDim.x, -SY/2];
        Expr *local_offset_x = nullptr;
        if (Acc->getSizeX() > 1) {
          local_offset_x = createBinaryOperator(Ctx, createIntegerLiteral(Ctx,
                (int32_t)i), tileVars.local_size_x, BO_Mul, Ctx.IntTy);
          global_offset_x = createBinaryOperator(Ctx, local_offset_x, SX2,
              BO_Sub, Ctx.IntTy);
        }

        stageLineToSharedMemory(PVD, stageBody, local_offset_x, nullptr,
            global_offset_x, global_offset_y);
      }
    }
  }
}


// stage data to shared memory for exploration
void ASTTranslate::stageIterationToSharedMemoryExploration(SmallVector<Stmt *,
    16> &stageBody) {
  for (auto I=kernelDecl->param_begin(), N=kernelDecl->param_end(); I!=N; ++I) {
    ParmVarDecl *PVD = *I;

    if (KernelDeclMapShared[PVD]) {
      HipaccAccessor *Acc = KernelDeclMapAcc[PVD];

      Expr *global_offset_x = nullptr, *global_offset_y = nullptr;
      Expr *SX2;
      SmallVector<Stmt *, 16> stageIter;
      VarDecl *iter = createVarDecl(Ctx, kernelDecl, "_N", Ctx.IntTy,
          createIntegerLiteral(Ctx, 0));
      DeclStmt *iter_stmt = createDeclStmt(Ctx, iter);
      DeclRefExpr *iter_ref = createDeclRefExpr(Ctx, iter);


      if (Acc->getSizeX() > 1) {
        if (compilerOptions.exploreConfig()) {
          SX2 = tileVars.local_size_x;
        } else {
          SX2 = createIntegerLiteral(Ctx, (int32_t)Kernel->getNumThreadsX());
        }
      } else {
        SX2 = createIntegerLiteral(Ctx, 0);
      }
      global_offset_y = createBinaryOperator(Ctx, iter_ref,
          tileVars.local_size_y, BO_Mul, Ctx.IntTy);
      if (Acc->getSizeY() > 1) {
        global_offset_y = createBinaryOperator(Ctx, global_offset_y,
            createUnaryOperator(Ctx, createIntegerLiteral(Ctx,
                (int32_t)Acc->getSizeY()/2), UO_Minus, Ctx.IntTy), BO_Add,
            Ctx.IntTy);
      }

      // check if we need to stage right apron
      size_t num_stages_x = 0;
      if (Acc->getSizeX() > 1) {
          num_stages_x = 2;
      }

      // load row (line)
      for (size_t i=0; i<=num_stages_x; ++i) {
        // _smem[lidYRef + N*(int)blockDim.y]
        //      [(int)threadIdx.x + i*(int)blockDim.x] =
        //        Image[-SX/2 + N*(int)blockDim.y + i*(int)blockDim.x, -SY/2];
        Expr *local_offset_x = nullptr;
        if (Acc->getSizeX() > 1) {
          local_offset_x = createBinaryOperator(Ctx, createIntegerLiteral(Ctx,
                (int32_t)i), tileVars.local_size_x, BO_Mul, Ctx.IntTy);
          global_offset_x = createBinaryOperator(Ctx, local_offset_x, SX2,
              BO_Sub, Ctx.IntTy);
        }

        stageLineToSharedMemory(PVD, stageIter, local_offset_x,
            createBinaryOperator(Ctx, iter_ref, tileVars.local_size_y, BO_Mul,
              Ctx.IntTy), global_offset_x, global_offset_y);
      }

      // PPT + (SY-2)/BSY + 1
      DeclRefExpr *DSY = createDeclRefExpr(Ctx, createVarDecl(Ctx, kernelDecl,
            "BSY_EXPLORE", Ctx.IntTy, nullptr));

      Expr *SY;
      if (Kernel->getPixelsPerThread() > 1) {
        SY = createIntegerLiteral(Ctx, (int32_t)Kernel->getPixelsPerThread());
      } else {
        SY = createIntegerLiteral(Ctx, 1);
      }

      if (Acc->getSizeY() > 1) {
        SY = createBinaryOperator(Ctx, SY, createBinaryOperator(Ctx,
              createBinaryOperator(Ctx, createIntegerLiteral(Ctx,
                  (int32_t)Acc->getSizeY()-2), DSY, BO_Div, Ctx.IntTy),
              createIntegerLiteral(Ctx, 1), BO_Add, Ctx.IntTy), BO_Add,
            Ctx.IntTy);
      }
      // for (int N=0; N < PPT*BSY + (SY-2)/BSY + 1)*BSY; N++)
      ForStmt *stageLoop = createForStmt(Ctx, iter_stmt,
          createBinaryOperator(Ctx, iter_ref, SY, BO_LT, Ctx.BoolTy),
          createUnaryOperator(Ctx, iter_ref, UO_PostInc, Ctx.IntTy),
          createCompoundStmt(Ctx, stageIter));

      stageBody.push_back(stageLoop);
    }
  }
}

// vim: set ts=2 sw=2 sts=2 et ai:

