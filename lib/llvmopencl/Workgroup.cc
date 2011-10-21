// LLVM module pass to create the single function (fully inlined)
// and parallelized kernel for an OpenCL workgroup.
// 
// Copyright (c) 2011 Universidad Rey Juan Carlos
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "BarrierTailReplication.h"
#include "WorkitemReplication.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/InstrTypes.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/TypeBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicInliner.h"
#include "llvm/Transforms/Utils/Local.h"
#include <map>

using namespace std;
using namespace llvm;
using namespace pocl;

static void noaliasArguments(Function *F);
static Function *createLauncher(Module &M, Function *F);
static void createWorkgroup(Module &M, Function *F);

extern cl::opt<string> Kernel;
extern cl::opt<string> Header;
extern cl::list<int> LocalSize;

namespace {
  class Workgroup : public ModulePass {
  
  public:
    static char ID;
    Workgroup() : ModulePass(ID) {}

    virtual bool runOnModule(Module &M);
  };

}

namespace llvm {

  typedef struct _pocl_context PoclContext;
  
  template<bool xcompile> class TypeBuilder<PoclContext, xcompile> {
  public:
    static StructType *get(LLVMContext &Context) {
        return StructType::get(
        TypeBuilder<types::i<32>, xcompile>::get(Context),
	TypeBuilder<types::i<32>[3], xcompile>::get(Context),
	TypeBuilder<types::i<32>[3], xcompile>::get(Context),
	TypeBuilder<types::i<32>[3], xcompile>::get(Context),
        NULL);
    }
  
    enum Fields {
      WORK_DIM,
      NUM_GROUPS,
      GROUP_ID,
      GLOBAL_OFFSET
    };
  };

  // template<bool xcompile> raw_ostream& operator<<(raw_ostream &OS, const TypeBuilder<PoclContext, xcompile> &T) {
  //   OS << "struct _pocl_context {\n";
  //   OS << "  cl_uint work_dim;\n";
  //   OS << "  cl_uint num_groups[3];\n";
  //   OS << "  cl_uint group_id[3];\n";
  //   OS << "  cl_uint global_offset[3];\n";
  //   OS << "};\n";
  //   return OS;
  // }
  
}  // namespace llvm
  
char Workgroup::ID = 0;
static RegisterPass<Workgroup> X("workgroup", "Workgroup creation pass");

bool
Workgroup::runOnModule(Module &M)
{
  BasicInliner BI;

  for (Module::iterator i = M.begin(), e = M.end(); i != e; ++i) {
    if (!i->isDeclaration())
      i->setLinkage(Function::InternalLinkage);
    BI.addFunction(i);
  }

  BI.inlineFunctions();

  BarrierTailReplication BTR;

  WorkitemReplication WR;

  string ErrorInfo;
  raw_fd_ostream out(Header.c_str(), ErrorInfo);

  NamedMDNode *SizeInfo = M.getNamedMetadata("opencl.kernel_wg_size_info");

  NamedMDNode *Kernels = M.getNamedMetadata("opencl.kernels");
  for (unsigned i = 0, e = Kernels->getNumOperands(); i != e; ++i) {
    Function *K = cast<Function>(Kernels->getOperand(i)->getOperand(0));

    if ((Kernel != "") && (K->getName() != Kernel))
      continue;

    out << "#define _" << K->getName() << "_NUM_LOCALS 0\n";
    out << "#define _" << K->getName() << "_LOCAL_SIZE {}\n";

    BTR.runOnFunction(*K);

    int OldLocalSize[3];
    for (int i = 0; i < 3; ++i)
      OldLocalSize[i] = LocalSize[i];;

    if (SizeInfo) {
      for (unsigned i = 0, e = SizeInfo->getNumOperands(); i != e; ++i) {
	MDNode *KernelSizeInfo = SizeInfo->getOperand(i);
	if (KernelSizeInfo->getOperand(0) == K) {
	  LocalSize[0] = (cast<ConstantInt>(KernelSizeInfo->getOperand(1)))->getLimitedValue();
	  LocalSize[1] = (cast<ConstantInt>(KernelSizeInfo->getOperand(2)))->getLimitedValue();
	  LocalSize[2] = (cast<ConstantInt>(KernelSizeInfo->getOperand(3)))->getLimitedValue();
	}
      }
    }
    WR.doInitialization(M);
    WR.runOnFunction(*K);
    for (int i = 0; i < 3; ++i)
      LocalSize[i] = OldLocalSize[i];;

    Function *L = createLauncher(M, K);

    L->addFnAttr(Attribute::NoInline);
    noaliasArguments(L);

    createWorkgroup(M, L);
  }

  return true;
}

static void
noaliasArguments(Function *F)
{
  // Argument 0 is return type, so add 1 to index here.
  for (unsigned i = 0, e = F->getFunctionType()->getNumParams(); i != e; ++i)
    F->setDoesNotAlias(i + 1);
}

static Function *
createLauncher(Module &M, Function *F)
{
  SmallVector<Type *, 8> v;

  for (Function::const_arg_iterator i = F->arg_begin(), e = F->arg_end();
       i != e; ++i)
    v.push_back (i->getType());
  v.push_back(TypeBuilder<PoclContext*, true>::get(M.getContext()));

  FunctionType *ft = FunctionType::get(Type::getVoidTy(M.getContext()),
				       ArrayRef<Type *> (v),
				       false);
  Function *L = Function::Create(ft,
				 Function::ExternalLinkage,
				 "_" + F->getNameStr(),
				 &M);

  SmallVector<Value *, 8> arguments;
  Function::arg_iterator ai = L->arg_begin();
  for (unsigned i = 0, e = F->getArgumentList().size(); i != e; ++i)  {
    arguments.push_back(ai);
    ++ai;
  }

  IRBuilder<> builder(BasicBlock::Create(M.getContext(), "", L));

  GlobalVariable *GroupID = M.getGlobalVariable("_group_id");
  if (GroupID != NULL) {
    Value *ptr = builder.CreateStructGEP(ai,
					 TypeBuilder<PoclContext, true>::GROUP_ID);
    Value *v = builder.CreateLoad(ptr);
    builder.CreateStore(v, GroupID);
  }

  GlobalVariable *NumGroups = M.getGlobalVariable("_num_groups");
  if (NumGroups != NULL) {
    Value *ptr = builder.CreateStructGEP(ai,
					 TypeBuilder<PoclContext, true>::NUM_GROUPS);
    Value *v = builder.CreateLoad(ptr);
    builder.CreateStore(v, NumGroups);
  }

  builder.CreateCall(F, ArrayRef<Value*>(arguments));
  builder.CreateRetVoid();
  
  return L;
  }

static void
createWorkgroup(Module &M, Function *F)
{
  IRBuilder<> builder(M.getContext());

  FunctionType *ft =
    TypeBuilder<void(types::i<8>*[],
		     PoclContext*), true>::get(M.getContext());

  Function *workgroup =
    dyn_cast<Function>(M.getOrInsertFunction(F->getNameStr() + "_workgroup", ft));
  assert(workgroup != NULL);

  builder.SetInsertPoint(BasicBlock::Create(M.getContext(), "", workgroup));

  Function::arg_iterator ai = workgroup->arg_begin();

  SmallVector<Value*, 8> arguments;
  int i = 0;
  for (Function::const_arg_iterator ii = F->arg_begin(), ee = F->arg_end();
       ii != ee; ++ii) {
    Type *t = ii->getType();
    
    Value *gep = builder.CreateGEP(ai, 
				   ConstantInt::get(IntegerType::get(M.getContext(), 32), i));
    Value *pointer = builder.CreateLoad(gep);
    Value *bc = builder.CreateBitCast(pointer, t->getPointerTo());
    arguments.push_back(builder.CreateLoad(bc));
    ++i;
  }

  arguments.back() = ++ai;
  
  builder.CreateCall(F, ArrayRef<Value*>(arguments));
  builder.CreateRetVoid();
}
