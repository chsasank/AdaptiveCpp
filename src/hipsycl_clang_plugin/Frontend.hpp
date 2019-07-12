/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2019 Aksel Alpay and contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HIPSYCL_FRONTEND_HPP
#define HIPSYCL_FRONTEND_HPP

#include <unordered_set>
#include <cassert>
#include <regex>
#include <sstream>

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/Attr.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/ASTMutationListener.h"
#include "clang/AST/DeclGroup.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Sema/Sema.h"

#include "CompilationState.hpp"
#include "Attributes.hpp"

#include "CL/sycl/detail/debug.hpp"

namespace hipsycl {

namespace detail {

///
/// Utility type to generate the set of all function declarations
/// implictly or explictly reachable from some initial declaration.
///
class CompleteCallSet : public clang::RecursiveASTVisitor<CompleteCallSet> {
  public:
    using FunctionSet = std::unordered_set<clang::FunctionDecl*>;

    CompleteCallSet(clang::Decl* D)
    {
      TraverseDecl(D);
    }

    bool VisitFunctionDecl(clang::FunctionDecl* FD)
    {
      visitedDecls.insert(FD);
      return true;
    }

    bool VisitCallExpr(clang::CallExpr* CE)
    {
      if(auto Callee = CE->getDirectCallee())
        TraverseDecl(Callee);
      return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* CE)
    {
      if(auto Callee = CE->getConstructor())
      {
        TraverseDecl(Callee);
        // Since for destructor calls no explicit AST nodes are created, we simply use this opportunity to
        // find the corresponding destructor for all constructed types (since we assume that every
        // type that can be constructed on the GPU also can and will be destructed).
        if(auto Ptr = llvm::dyn_cast_or_null<clang::PointerType>(Callee->getThisType()->getCanonicalTypeUnqualified()))
          if(auto Record = llvm::dyn_cast<clang::RecordType>(Ptr->getPointeeType()))
            if(auto RecordDecl = llvm::dyn_cast<clang::CXXRecordDecl>(Record->getDecl()))
              if(auto DtorDecl = RecordDecl->getDestructor())
                TraverseDecl(DtorDecl);
      }
      return true;
    }

    bool TraverseDecl(clang::Decl* D)
    {
      if(visitedDecls.find(llvm::dyn_cast_or_null<clang::FunctionDecl>(D)) == visitedDecls.end())
        return clang::RecursiveASTVisitor<CompleteCallSet>::TraverseDecl(D);
      return true;
    }

    bool shouldWalkTypesOfTypeLocs() const { return false; }
    bool shouldVisitTemplateInstantiations() const { return true; }
    bool shouldVisitImplicitCode() const { return true; }

    const FunctionSet& getReachableDecls() const { return visitedDecls; }

  private:
    FunctionSet visitedDecls;
};

///
/// Builds a kernel name from a RecordDecl, taking into account template specializations.
/// Returns an empty string if the name is not a valid kernel name.
///
inline std::string buildKernelNameFromRecordDecl(const clang::RecordDecl *Decl) {
  std::stringstream SS;
  SS << "$" << Decl->getNameAsString();

  if(auto TD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(Decl))
  {
    for(auto arg : TD->getTemplateArgs().asArray())
    {
      switch (arg.getKind())
      {
        case clang::TemplateArgument::Type:
        {
          if(arg.getAsType().getTypePtr()->getAsCXXRecordDecl() &&
             arg.getAsType().getTypePtr()->getAsCXXRecordDecl()->isLambda())
          {
            return ""; // Lambdas are not supported
          }

          auto RecordType = llvm::dyn_cast<clang::RecordType>(arg.getAsType().getTypePtr());
          auto RecordDecl = llvm::dyn_cast_or_null<clang::RecordDecl>(
            RecordType ? RecordType->getDecl() : nullptr);

          if(RecordDecl)
          {
            auto declName = buildKernelNameFromRecordDecl(RecordDecl);
            if(declName.empty()) return "";
            SS << "_" << declName;
          }
          else
          {
            SS << "_" << arg.getAsType().getAsString();
          }
          break;
        }
        case clang::TemplateArgument::Integral:
          SS << "_" << arg.getAsIntegral().toString(10);
          break;
        case clang::TemplateArgument::NullPtr:
          SS << "_nullptr";
          break;
        case clang::TemplateArgument::Template:
        {
          std::string QualifiedName = arg.getAsTemplate().getAsTemplateDecl()->getTemplatedDecl()->getQualifiedNameAsString();
          SS << "__" << std::regex_replace(QualifiedName, std::regex("::"), "__");
          break;
        }
        default: return ""; // Everything else is not supported
      }
    }
  }

  return SS.str();
}

inline std::string buildKernelName(clang::TemplateArgument SyclTagType) {
  assert(SyclTagType.getKind() == clang::TemplateArgument::ArgKind::Type);
  auto RecordType = llvm::dyn_cast<clang::RecordType>(SyclTagType.getAsType().getTypePtr());
  if(RecordType == nullptr || !llvm::isa<clang::RecordDecl>(RecordType->getDecl()))
  {
    // We only support structs/classes as kernel names
    return "";
  }
  auto DeclName = buildKernelNameFromRecordDecl(RecordType->getAsRecordDecl());
  if(DeclName.empty()) return "";
  return "__hipsycl_kernel_" + DeclName;
}

}

class FrontendASTVisitor : public clang::RecursiveASTVisitor<FrontendASTVisitor>
{
  clang::CompilerInstance &Instance;
  clang::MangleContext* MangleContext;
public:
  FrontendASTVisitor(clang::CompilerInstance& instance)
    : Instance{instance}
  {
    this->MangleContext = Instance.getASTContext().createMangleContext();
  }

  ~FrontendASTVisitor()
  {
    delete this->MangleContext;
  }
  
  bool shouldVisitTemplateInstantiations() const { return true; }

  /// Return whether this visitor should recurse into implicit
  /// code, e.g., implicit constructors and destructors.
  bool shouldVisitImplicitCode() const { return true; }
  
  // We also need to have look at all statements to identify Lambda declarations
  bool VisitStmt(clang::Stmt *S) {

    if(clang::isa<clang::LambdaExpr>(S))
    {
      clang::LambdaExpr* lambda = clang::cast<clang::LambdaExpr>(S);
      clang::FunctionDecl* callOp = lambda->getCallOperator();
      if(callOp)
        this->VisitFunctionDecl(callOp);
    }
    
    return true;
  }
  
  bool VisitFunctionDecl(clang::FunctionDecl *f) {
    if(!f)
      return true;
    
    this->processFunctionDecl(f);

    return true;
  }

  bool VisitCallExpr(clang::CallExpr *Call) {
    auto F = llvm::dyn_cast_or_null<clang::FunctionDecl>(Call->getDirectCallee());
    if(!F) return true;
    if(!CustomAttributes::SyclKernel.isAttachedTo(F)) return true;

    auto KernelFunctorType = llvm::dyn_cast<clang::RecordType>(Call->getArg(0)
      ->getType()->getCanonicalTypeUnqualified());

    // Store user kernel for it to be marked as device code later on
    if(KernelFunctorType)
    {
      auto Methods = llvm::dyn_cast<clang::CXXRecordDecl>(
        KernelFunctorType->getDecl())->methods();
      for(auto&& M : Methods)
      {
        if(M->getNameAsString() == "operator()")
          UserKernels.insert(M);
      }
    }

    // Determine unique kernel name to be used for symbol name in device IR
    clang::FunctionTemplateSpecializationInfo* Info = F->getTemplateSpecializationInfo();
    auto KernelName = detail::buildKernelName(Info->TemplateArguments->get(0));

    // Abort with error diagnostic if no kernel name could be built
    if(KernelName.empty())
    {
      // Since we cannot easily get the source location of the template
      // specialization where the name is passed by the user (e.g. a
      // parallel_for call), we attach the diagnostic to the kernel
      // functor instead.
      // TODO: Improve on this.
      auto SL = llvm::dyn_cast<clang::CXXRecordDecl>(
        KernelFunctorType->getDecl())->getSourceRange().getBegin();
      auto ID = Instance.getASTContext().getDiagnostics()
        .getCustomDiagID(clang::DiagnosticsEngine::Level::Error,
          "Not a valid kernel name: %0");
      Instance.getASTContext().getDiagnostics().Report(SL, ID) <<
        Info->TemplateArguments->get(0);
    }

    // Add the AsmLabel attribute which, if present,
    // is used by Clang instead of the function's mangled name.
    F->addAttr(clang::AsmLabelAttr::CreateImplicit(Instance.getASTContext(),
      KernelName));
    HIPSYCL_DEBUG_INFO << "AST processing: Adding ASM label attribute with kernel name "
      << KernelName << "\n";

    return true;
  }

  void applyAttributes()
  {
    for(auto F : MarkedHostDeviceFunctions)
    {
      // Strictly speaking, setting these attributes is not even necessary!
      // It's only important that the kernel has the right attribute.
      if (!F->hasAttr<clang::CUDAHostAttr>())
        F->addAttr(clang::CUDAHostAttr::CreateImplicit(Instance.getASTContext()));
      if (!F->hasAttr<clang::CUDADeviceAttr>())
        F->addAttr(clang::CUDADeviceAttr::CreateImplicit(Instance.getASTContext()));
    }

    for(auto F : MarkedKernels)
    {
      if (!F->hasAttr<clang::CUDAGlobalAttr>() &&
          CustomAttributes::SyclKernel.isAttachedTo(F)) {

        auto* NewAttr = clang::CUDAGlobalAttr::CreateImplicit(Instance.getASTContext());
        
        F->addAttr(NewAttr);
      }
    }

    for(auto F : UserKernels)
    {
      std::unordered_set<clang::FunctionDecl*> UserKernels;

      // Mark all functions called by user kernels as host / device.
      detail::CompleteCallSet CCS(F);
      for (auto&& RD : CCS.getReachableDecls())
      {
        HIPSYCL_DEBUG_INFO << "AST processing: Marking function as __host__ __device__: "
                           << RD->getQualifiedNameAsString() << std::endl;
        CompilationStateManager::getASTPassState().addImplicitHostDeviceFunction(getMangledName(RD));
        markAsHostDevice(RD);
        if (!RD->hasAttr<clang::CUDAHostAttr>())
          RD->addAttr(clang::CUDAHostAttr::CreateImplicit(Instance.getASTContext()));
        if (!RD->hasAttr<clang::CUDADeviceAttr>())
          RD->addAttr(clang::CUDADeviceAttr::CreateImplicit(Instance.getASTContext()));
      }
    }
  }

  std::unordered_set<clang::FunctionDecl*>& getMarkedHostDeviceFunctions()
  {
    return MarkedHostDeviceFunctions;
  }

  std::unordered_set<clang::FunctionDecl*>& getKernels()
  {
    return MarkedKernels;
  }

private:
  std::unordered_set<clang::FunctionDecl*> MarkedHostDeviceFunctions;
  std::unordered_set<clang::FunctionDecl*> MarkedKernels;
  std::unordered_set<clang::FunctionDecl*> UserKernels;

  void markAsHostDevice(clang::FunctionDecl* F)
  {
    this->MarkedHostDeviceFunctions.insert(F);
  }

  void markAsKernel(clang::FunctionDecl* F)
  {
    this->MarkedKernels.insert(F);
  }

  void processFunctionDecl(clang::FunctionDecl* f)
  {
    if(!f)
      return;

    if(f->getQualifiedNameAsString() 
        == "cl::sycl::detail::dispatch::device::parallel_for_workgroup")
    {
      clang::FunctionDecl* Kernel = 
        this->getKernelFromHierarchicalParallelFor(f);

      if (Kernel) 
      {
        HIPSYCL_DEBUG_INFO << "AST Processing: Detected parallel_for_workgroup kernel "
                          << Kernel->getQualifiedNameAsString() << std::endl;
                          
        this->storeLocalVariablesInLocalMemory(Kernel);
      }
    }
  
    std::string MangledName = getMangledName(f);
    if(CustomAttributes::SyclKernel.isAttachedTo(f))
    {
      markAsKernel(f); 
      CompilationStateManager::getASTPassState().addKernelFunction(MangledName);
    }
    else if(f->hasAttr<clang::CUDADeviceAttr>())
    {
      if(!f->getAttr<clang::CUDADeviceAttr>()->isImplicit())
        CompilationStateManager::getASTPassState().addExplicitDeviceFunction(MangledName);
    }
    else if(f->hasAttr<clang::CUDAGlobalAttr>())
    {
      CompilationStateManager::getASTPassState().addKernelFunction(MangledName);
    }
  }

  clang::FunctionDecl* getKernelFromHierarchicalParallelFor(
    clang::FunctionDecl* KernelDispatch) const
  {
    if (auto *B = KernelDispatch->getBody()) 
    {
      for (auto S = B->child_begin();
          S != B->child_end(); ++S)
      {
        if(auto* C = clang::dyn_cast<clang::CallExpr>(*S))
        {
          if(clang::FunctionDecl* F = C->getDirectCallee())
            return F;
        }
      }
    }
    return nullptr;
  }


  bool isPrivateMemory(const clang::VarDecl* V) const
  {
    const clang::CXXRecordDecl* R = V->getType()->getAsCXXRecordDecl();
    if(R)
      return R->getQualifiedNameAsString() == "cl::sycl::private_memory";
  
    return false;
  }

  void storeLocalVariablesInLocalMemory(clang::FunctionDecl* F) const
  {
    if(auto* Body = F->getBody())
    {
      // TODO Actually we need to this recursively for nested statements
      for(auto CurrentStmt = Body->child_begin();
          CurrentStmt != Body->child_end(); ++CurrentStmt)
      {
        if(clang::DeclStmt* D = clang::dyn_cast<clang::DeclStmt>(*CurrentStmt))
        {
          for(auto decl = D->decl_begin();
              decl != D->decl_end();
              ++decl)
          {
            if(clang::VarDecl* V = clang::dyn_cast<clang::VarDecl>(*decl))
            {
              if (!isPrivateMemory(V)) 
              {
                HIPSYCL_DEBUG_INFO
                    << "AST Processing: Marking variable as __shared__ in "
                    << F->getAsFunction()->getQualifiedNameAsString()
                    << std::endl;
                if (!V->hasAttr<clang::CUDASharedAttr>()) {
                  V->addAttr(clang::CUDASharedAttr::CreateImplicit(
                      Instance.getASTContext()));
                  V->setStorageClass(clang::SC_Static);
                }
              }
              
            }
          }
        }
      }
    }
  }
  

  std::string getMangledName(clang::FunctionDecl* decl)
  {
    if (!MangleContext->shouldMangleDeclName(decl)) {
      return decl->getNameInfo().getName().getAsString();
    }

    std::string mangledName;
    llvm::raw_string_ostream ostream(mangledName);

    MangleContext->mangleName(decl, ostream);

    ostream.flush();

    return mangledName;
  }
};




class FrontendASTConsumer : public clang::ASTConsumer {
  
  FrontendASTVisitor Visitor;
  clang::CompilerInstance& Instance;
  
public:
  FrontendASTConsumer(clang::CompilerInstance &I)
      : Visitor{I}, Instance{I}
  {
    CompilationStateManager::get().reset();
  }

  bool HandleTopLevelDecl(clang::DeclGroupRef DG) override {
    for (auto&& D : DG)
      Visitor.TraverseDecl(D);
    return true;
  }

  void HandleTranslationUnit(clang::ASTContext& context) override {
    
    CompilationStateManager::getASTPassState().setDeviceCompilation(
        Instance.getSema().getLangOpts().CUDAIsDevice);

    if(CompilationStateManager::getASTPassState().isDeviceCompilation())
      HIPSYCL_DEBUG_INFO << " ****** Entering compilation mode for __device__ ****** " << std::endl;
    else
      HIPSYCL_DEBUG_INFO << " ****** Entering compilation mode for __host__ ****** " << std::endl;

    Visitor.applyAttributes();

    // The following part is absolutely crucial:
    //
    // clang works roughly like this when building and processing the AST:
    // while(!not done){
    //   DeclGroupRef DG = parseNextDeclGroup()
    //   foreach(ASTConsumer C){
    //     C->HandleTopLevelDecl(DG);
    //   }
    // }
    // }
    // foreach(ASTConsumer C){
    //   C->HandleTranslationUnit(TU);
    // }
    //
    // The BackendConsumers which take care of emitting IR code
    // already emit in HandleTopLevelDecl().
    // This means that, since we have only made attribute changes in
    // HandleTranslationUnit(), all code has already been emitted without taking
    // into account our changes. In particular, since functions used in SYCL
    // kernels hadn't yet been marked as __device__ at this point, none of them
    // actually got emitted. To fix this, we let all registered ASTConsumers run
    // their HandleTopLevelDecl() over the functions where we have added
    // attributes. Since we do not implement HandleTopLevelDecl(), the only
    // consumers affected are the BackendConsumers which will then generate the
    // required IR for device code.
    if(CompilationStateManager::getASTPassState().isDeviceCompilation()){
      clang::ASTConsumer& C = Instance.getASTConsumer();
      if(clang::isa<clang::MultiplexConsumer>(&C))
      {
        clang::MultiplexConsumer& MC = static_cast<clang::MultiplexConsumer&>(C);

        for (clang::FunctionDecl *HDFunction :
            Visitor.getMarkedHostDeviceFunctions()) {
          clang::DeclGroupRef DG{HDFunction};

          MC.HandleTopLevelDecl(DG);
        }
        for(clang::FunctionDecl* Kernel : Visitor.getKernels()){
          clang::DeclGroupRef DG{Kernel};

          MC.HandleTopLevelDecl(DG);
        }
      }
    }
  }
};

}

#endif
