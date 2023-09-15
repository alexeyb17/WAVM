#include <stddef.h>
#include <memory>
#include <vector>
#include "EmitFunctionContext.h"
#include "EmitModuleContext.h"
#include "LLVMJITPrivate.h"
#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Signal.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;
using namespace WAVM::Runtime;

// Essential rules for Itanium C++ ABI exception handling (https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html)
//
// 1. `cleanup` landing pad must call _Unwind_Resume at the end of.
// 2. `catch` landing pad must call __cxa_begin_catch().
// 3. Each __cxa_begin_catch() must be closed with __cxa_end_catch(). So, exception object can be
//    destroyed at the end of handling.
// 4. Exception can be rethrowm with __cxa_rethrow() after __cxa_begin_catch() call. Still
//    __cxa_end_catch() must be called in the destintation landing pad for rethrow invoke.
// 5. __cxa_end_catch() called after __cxa_rethrow() doesn't destroy exception, just removes
//    it from list of caught exceptions. So, __cxa_begin_catch() can be called again to start new handler.
// 6. Propagating exception to the caller can be done in following ways:
//    a) When no try/catch blocks exist at all: doesn't use `invoke` at all.
//    b) When inside catch handler(s) but not in any `try` block: use `cleanup` landing pad that ends
//       active catch handlers and call _Unwind_Resume.


static llvm::Function* getCXARethrowFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaRethrowFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext), false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_rethrow",
			moduleContext.llvmModule);
	}
	return rv;
}

llvm::Function* getCXAGetExceptionPtrFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaGetExceptionPtrFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvmContext.i8PtrType, {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_get_exception_ptr",
			moduleContext.llvmModule);
	}
	return rv;
}

static llvm::Function* getCXABeginCatchFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaBeginCatchFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvmContext.i8PtrType, {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_begin_catch",
			moduleContext.llvmModule);
	}
	return rv;
}

static llvm::Function* getCXAEndCatchFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaEndCatchFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext), false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_end_catch",
			moduleContext.llvmModule);
	}
	return rv;
}

llvm::BasicBlock* EmitFunctionContext::getUnreachableBlock()
{
	auto& block = unreachableBlock;
	if (!block)
	{
		InsertPointGuard savedIp(irBuilder);
		block = llvm::BasicBlock::Create(llvmContext, "unreachable", function);
		irBuilder.SetInsertPoint(block);
		irBuilder.CreateUnreachable();
	}
	return block;
}

void EmitFunctionContext::endTryCatch()
{
	ControlContext& currentContext = controlStack.back();
	WAVM_ASSERT(currentContext.type == ControlContext::Type::try_
				|| currentContext.type == ControlContext::Type::catch_
				|| currentContext.type == ControlContext::Type::catch_all);

	CatchContext& catchContext = currentContext.catchContext;

	if (currentContext.type == ControlContext::Type::catch_
	   || currentContext.type == ControlContext::Type::catch_all)
	{
		exitCatch();
	}
	else
	{
		// try without catch - finalize created landpad(s) if any
		finalizeLandingPads(catchContext);
		// mimic we are in catch rest of the path
		currentContext.type = ControlContext::Type::catch_;
	}

	if (!catchContext.nextHandlerBlock)
	{
		// No exceptions are generated at all
		return;
	}

	// If an end instruction terminates a sequence of catch clauses, terminate the chain of
	// handler type ID tests by joining outer catch handler chain or by rethrowing the exception
	// to the caller.

	auto propagateTo = getInnermostTry();
	if (propagateTo.controlContext)
	{
		// redirect to outer try block
		propagateTo.controlContext->catchContext.exceptionPointers.push_back(
			CatchContext::ExceptionPointer{catchContext.exceptionPointer, catchContext.nextHandlerBlock});
	}
	else
	{
		InsertPointGuard savedIp(irBuilder);
		irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
		irBuilder.CreateInvoke(getCXARethrowFunction(moduleContext), getUnreachableBlock(),
							   createUnwindResumePad(propagateTo.catchCount));
	}
}

void EmitFunctionContext::exitCatch()
{
	ControlContext& currentContext = controlStack.back();
	WAVM_ASSERT(currentContext.type == ControlContext::Type::catch_
				|| currentContext.type == ControlContext::Type::catch_all);

	if (currentContext.isReachable)
	{
		irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
	}
}

EmitFunctionContext::UnwindPath EmitFunctionContext::getInnermostTry()
{
	UnwindPath rv;
	for (auto it = controlStack.rbegin(); it != controlStack.rend(); ++it)
	{
		if (it->type == ControlContext::Type::try_)
		{
			rv.controlContext = &(*it);
			return rv;
		}
		else if (it->type == ControlContext::Type::catch_ || it->type == ControlContext::Type::catch_all)
		{
			++rv.catchCount;
		}
	}
	return rv;
}

llvm::BasicBlock* EmitFunctionContext::getInnermostUnwindToBlock()
{
	auto target = getInnermostTry();
	if (target.controlContext == nullptr && target.catchCount == 0)
	{
		// no handlers in this function and no inner catches
		return nullptr;
	}
	if (target.controlContext == nullptr)
	{
		// no handlers, but have to cleanup active catches
		return createUnwindResumePad(target.catchCount);
	}
	else
	{
		return createCatchPad(target.controlContext->catchContext, target.catchCount);
	}
}

llvm::BasicBlock* EmitFunctionContext::createUnwindResumePad(Uptr numActiveCatches)
{
	WAVM_ASSERT(numActiveCatches > 0);
	auto& landingPadBlock = unwindResumePads[numActiveCatches];

	if (landingPadBlock)
	{
		return landingPadBlock;
	}
	InsertPointGuard savedIp(irBuilder);

	landingPadBlock = llvm::BasicBlock::Create(llvmContext, "cleanupAndResume", function);
	irBuilder.SetInsertPoint(landingPadBlock);
	auto cleanupLandingPad = irBuilder.CreateLandingPad(
		llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
	cleanupLandingPad->setCleanup(true);
	for (Uptr i = 0; i != numActiveCatches; ++i)
	{
		irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
	}
	irBuilder.CreateResume(cleanupLandingPad);
	return landingPadBlock;

}

llvm::BasicBlock* EmitFunctionContext::createCatchPad(CatchContext& catchContext, Uptr numActiveCatches)
{
	// Create `landingpad` if not yet.
	auto& landingPadBlock = catchContext.unwindToBlock[numActiveCatches];
	if (!landingPadBlock)
	{
		InsertPointGuard savedIp(irBuilder);

		landingPadBlock = llvm::BasicBlock::Create(llvmContext, "landingPad", function);
		irBuilder.SetInsertPoint(landingPadBlock);
		auto landingPadInst = irBuilder.CreateLandingPad(
			llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
		landingPadInst->addClause(moduleContext.runtimeExceptionTypeInfo);

			   // Cleanup inner catches before starting to process caught exception.
		for (Uptr i = 0; i != numActiveCatches; ++i)
		{
			irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
		}
		// Call __cxa_begin_catch to get the exception pointer.
		auto exceptionPointer
			= irBuilder.CreateCall(getCXABeginCatchFunction(moduleContext),
								   {irBuilder.CreateExtractValue(landingPadInst, {0})});
		catchContext.exceptionPointers.push_back(
			CatchContext::ExceptionPointer{exceptionPointer, landingPadBlock});
	}
	return landingPadBlock;
}

void EmitFunctionContext::finalizeLandingPads(CatchContext& catchContext)
{
	WAVM_ASSERT(catchContext.nextHandlerBlock == nullptr);

	InsertPointGuard savedIp(irBuilder);

	if (catchContext.exceptionPointers.empty())
	{
		// no exceptions are expected at all, nothing to do
		return;
	}
	if (catchContext.exceptionPointers.size() == 1)
	{
		// Single path leading to this catch. Can continue in this block.
		catchContext.nextHandlerBlock = catchContext.exceptionPointers[0].block;
		catchContext.exceptionPointer = catchContext.exceptionPointers[0].value;
	}
	else
	{
		// collect results of several landing pads
		auto phiBlock = llvm::BasicBlock::Create(llvmContext, "landingPadPhi", function);
		irBuilder.SetInsertPoint(phiBlock);
		auto phiInst = irBuilder.CreatePHI(llvmContext.i8PtrType, catchContext.exceptionPointers.size());
		for (const auto& ep: catchContext.exceptionPointers)
		{
			phiInst->addIncoming(ep.value, ep.block);
			irBuilder.SetInsertPoint(ep.block);
			irBuilder.CreateBr(phiBlock);
		}
		catchContext.nextHandlerBlock = phiBlock;
		catchContext.exceptionPointer = phiInst;
	}
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	// load type ID from Exception object
	catchContext.exceptionTypeId = loadFromUntypedPointer(
		createInBoundsGEP(
			llvmContext.i8Type,
			catchContext.exceptionPointer,
			{emitLiteralIptr(offsetof(Exception, typeId), moduleContext.iptrType)}),
		moduleContext.iptrType);
}

void EmitFunctionContext::endAllCatches()
{
	for (auto& controlContext: controlStack)
	{
		if (controlContext.isReachable &&
		   (controlContext.type == ControlContext::Type::catch_
			|| controlContext.type == ControlContext::Type::catch_all))
		{
			irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
		}
	}
}

void EmitFunctionContext::try_(ControlStructureImm imm)
{
	auto originalInsertBlock = irBuilder.GetInsertBlock();

	CatchContext catchContext;
	if (moduleContext.useWindowsSEH)
	{
#if 0
		// Insert an alloca for the exception pointer at the beginning of the function.
		irBuilder.SetInsertPoint(&function->getEntryBlock(),
								 function->getEntryBlock().getFirstInsertionPt());
		llvm::Value* exceptionPointerAlloca
			= irBuilder.CreateAlloca(llvmContext.i8PtrType, nullptr, "exceptionPointer");

		// Create a BasicBlock with a CatchSwitch instruction to use as the unwind target.
		auto catchSwitchBlock = llvm::BasicBlock::Create(llvmContext, "catchSwitch", function);
		irBuilder.SetInsertPoint(catchSwitchBlock);
		auto catchSwitchInst
			= irBuilder.CreateCatchSwitch(llvm::ConstantTokenNone::get(llvmContext), nullptr, 1);

		// Create a block+catchpad that the catchswitch will transfer control if the exception type
		// info matches a WAVM runtime exception.
		auto catchPadBlock = llvm::BasicBlock::Create(llvmContext, "catchPad", function);
		catchSwitchInst->addHandler(catchPadBlock);
		irBuilder.SetInsertPoint(catchPadBlock);
		auto catchPadInst = irBuilder.CreateCatchPad(catchSwitchInst,
													 {moduleContext.runtimeExceptionTypeInfo,
													  emitLiteral(llvmContext, I32(0)),
													  exceptionPointerAlloca});

		// Create a catchret that immediately returns from the catch "funclet" to a new non-funclet
		// basic block.
		auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
		irBuilder.CreateCatchRet(catchPadInst, catchBlock);
		irBuilder.SetInsertPoint(catchBlock);

		// Load the exception pointer from the alloca that the catchpad wrote it to.
		// SEH NOT WORKING - rewrite
		auto exceptionPointer
			= loadFromUntypedPointer(exceptionPointerAlloca, llvmContext.i8PtrType);

		// Load the exception type ID.
		auto exceptionTypeId = loadFromUntypedPointer(
			createInBoundsGEP(
				llvmContext.i8Type,
				exceptionPointer,
				{emitLiteralIptr(offsetof(Exception, typeId), moduleContext.iptrType)}),
			moduleContext.iptrType);

		catchContext.unwindToBlock = catchSwitchBlock;
		catchContext.catchSwitchInst = catchSwitchInst;
		catchContext.exceptionPointer = exceptionPointer;
		catchContext.nextHandlerBlock = catchBlock;
		catchContext.exceptionTypeId = exceptionTypeId;
#else
		WAVM_ASSERT(false && "SEH not implemented");
#endif
	}
	else
	{
		// CatchContext is filled on-demand
	}

	irBuilder.SetInsertPoint(originalInsertBlock);

	// Create an end try+phi for the try result.
	FunctionType blockType = resolveBlockType(irModule, imm.type);
	auto endBlock = llvm::BasicBlock::Create(llvmContext, "tryEnd", function);
	auto endPHIs = createPHIs(endBlock, blockType.results());

	// Pop the try arguments.
	llvm::Value** tryArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * blockType.params().size());
	popMultiple(tryArgs, blockType.params().size());

	// Push a control context that ends at the end block/phi.
	pushControlStack(ControlContext::Type::try_, blockType.results(), endBlock, endPHIs);
	controlStack.back().catchContext = catchContext;

	// Push a branch target for the end block/phi.
	pushBranchTarget(blockType.results(), endBlock, endPHIs);

	// Repush the try arguments.
	pushMultiple(tryArgs, blockType.params().size());
}

void EmitFunctionContext::catch_(ExceptionTypeImm imm)
{
	WAVM_ASSERT(controlStack.size());
	ControlContext& controlContext = controlStack.back();
	CatchContext& catchContext = controlContext.catchContext;
	WAVM_ASSERT(controlContext.type == ControlContext::Type::try_
				|| controlContext.type == ControlContext::Type::catch_);
	if(controlContext.type == ControlContext::Type::try_)
	{
		branchToEndOfControlContext();
		controlContext.type = ControlContext::Type::catch_;
		controlContext.isReachable = true;
		finalizeLandingPads(catchContext);
	}
	else
	{
		exitCatch();
		branchToEndOfControlContext();
		controlContext.isReachable = true;
	}

	// Look up the exception type instance to be caught
	WAVM_ASSERT(imm.exceptionTypeIndex < moduleContext.exceptionTypeIds.size());
	const auto& tagType = irModule.tags.getType(imm.exceptionTypeIndex);
	WAVM_ASSERT(tagType.index < irModule.types.size());
	const auto& exceptionParams = irModule.types[tagType.index].params();
	llvm::Constant* catchTypeId = moduleContext.exceptionTypeIds[imm.exceptionTypeIndex];

	if (!catchContext.nextHandlerBlock)
	{
		// No exceptions are expected - this catch is never reached.
		irBuilder.SetInsertPoint(llvm::BasicBlock::Create(llvmContext, "unreachableCatch", function));
		// Still we have to push fake exception arguments on the stack to make rest of code generator happy.
		for(Uptr argumentIndex = 0; argumentIndex < exceptionParams.size(); ++argumentIndex)
		{
			const ValueType parameters = exceptionParams[argumentIndex];
			auto zeroVal = llvm::Constant::getNullValue(asLLVMType(llvmContext, parameters));
			push(zeroVal);
		}
	}
	else
	{
		irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);

		auto isExceptionType = irBuilder.CreateICmpEQ(catchContext.exceptionTypeId, catchTypeId);

		auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
		auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "nextCatch", function);
		irBuilder.CreateCondBr(isExceptionType, catchBlock, unhandledBlock);
		catchContext.nextHandlerBlock = unhandledBlock;
		irBuilder.SetInsertPoint(catchBlock);

		auto argumentsPtr = loadFromUntypedPointer(
			createInBoundsGEP(llvmContext.i8Type, catchContext.exceptionPointer,
							  {emitLiteral(llvmContext, offsetof(Exception, arguments))}),
			llvmContext.i8PtrType);
		// Push the exception arguments on the stack.
		for(Uptr argumentIndex = 0; argumentIndex < exceptionParams.size(); ++argumentIndex)
		{
			const ValueType parameters = exceptionParams[argumentIndex];
			const Uptr argOffset
				= (exceptionParams.size() - argumentIndex - 1) * sizeof(Exception::arguments[0]);
			auto argument = loadFromUntypedPointer(
				createInBoundsGEP(llvmContext.i8Type, argumentsPtr, {emitLiteral(llvmContext, argOffset)}),
				asLLVMType(llvmContext, parameters),
				sizeof(Exception::arguments[0]));
			push(argument);
		}
	}
}

void EmitFunctionContext::catch_all(NoImm)
{
	WAVM_ASSERT(controlStack.size());
	ControlContext& controlContext = controlStack.back();
	CatchContext& catchContext = controlContext.catchContext;
	WAVM_ASSERT(controlContext.type == ControlContext::Type::try_
				|| controlContext.type == ControlContext::Type::catch_);
	if(controlContext.type == ControlContext::Type::try_)
	{
		branchToEndOfControlContext();
		controlContext.type = ControlContext::Type::catch_all;
		controlContext.isReachable = true;
		finalizeLandingPads(catchContext);
	}
	else
	{
		exitCatch();
		branchToEndOfControlContext();
		controlContext.type = ControlContext::Type::catch_all;
		controlContext.isReachable = true;
	}

	if (!catchContext.nextHandlerBlock)
	{
		// No exceptions are expected - this catch is never reached.
		irBuilder.SetInsertPoint(llvm::BasicBlock::Create(llvmContext, "unreachableCatch", function));
		return;
	}
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);

	// TODO: Do we want to catch non-user exceptions to perform cleanup?
	auto isUserExceptionType = irBuilder.CreateICmpNE(
		loadFromUntypedPointer(
			createInBoundsGEP(
				llvmContext.i8Type,
				catchContext.exceptionPointer,
				{emitLiteralIptr(offsetof(Exception, isUserException), moduleContext.iptrType)}),
			llvmContext.i8Type),
		llvm::ConstantInt::get(llvmContext.i8Type, llvm::APInt(8, 0, false)));

	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isUserExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);
}

void EmitFunctionContext::throw_(ExceptionTypeImm imm)
{
	WAVM_ASSERT(imm.exceptionTypeIndex < moduleContext.exceptionTypeIds.size());
	const auto& tagType = irModule.tags.getType(imm.exceptionTypeIndex);
	WAVM_ASSERT(tagType.index < irModule.types.size());
	const auto& exceptionParams = irModule.types[tagType.index].params();

	const Uptr numArgs = exceptionParams.size();
	const Uptr numArgBytes = numArgs * sizeof(UntaggedValue);
	auto argBaseAddress
		= irBuilder.CreateAlloca(llvmContext.i8Type, emitLiteral(llvmContext, numArgBytes));
	argBaseAddress->setAlignment(LLVM_ALIGNMENT(sizeof(UntaggedValue)));

	for(Uptr argIndex = 0; argIndex < exceptionParams.size(); ++argIndex)
	{
		auto elementValue = pop();
		storeToUntypedPointer(
			elementValue,
			irBuilder.CreatePointerCast(
				createInBoundsGEP(
					llvmContext.i8Type,
					argBaseAddress,
					{emitLiteral(llvmContext, (numArgs - argIndex - 1) * sizeof(UntaggedValue))}),
				elementValue->getType()->getPointerTo()),
			sizeof(UntaggedValue));
	}

	llvm::Value* exceptionTypeId = moduleContext.exceptionTypeIds[imm.exceptionTypeIndex];
	llvm::Value* argsPointerAsInt
		= irBuilder.CreatePtrToInt(argBaseAddress, moduleContext.iptrType);

	emitRuntimeIntrinsic(
		"throwException",
		FunctionType(
			TypeTuple{moduleContext.iptrValueType},
			TypeTuple{moduleContext.iptrValueType, moduleContext.iptrValueType, ValueType::i32},
			IR::CallingConvention::intrinsic),
		{exceptionTypeId, argsPointerAsInt, emitLiteral(llvmContext, I32(1))})[0];

	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::rethrow(RethrowImm imm)
{
	WAVM_ASSERT(imm.catchDepth < controlStack.size());
	auto targetDepth = controlStack.size() - imm.catchDepth - 1;
	auto& controlContext = controlStack[targetDepth];
	WAVM_ASSERT(controlContext.type == ControlContext::Type::catch_
				|| controlContext.type == ControlContext::Type::catch_all);

	auto target = getInnermostTry();

	// End all innermost catch blocks before rethrowing exception pointed by `rethrow imm`.
	for (Uptr depth = controlStack.size() - 1; depth > targetDepth; --depth)
	{
		auto& innerContext = controlStack[depth];
		if (innerContext.type == ControlContext::Type::catch_
		   || innerContext.type == ControlContext::Type::catch_all)
		{
			irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
			WAVM_ASSERT(target.catchCount > 0);
			--target.catchCount;
		}
	}

	auto landingPad = target.controlContext ? createCatchPad(target.controlContext->catchContext, target.catchCount)
											: createUnwindResumePad(target.catchCount);
	irBuilder.CreateInvoke(getCXARethrowFunction(moduleContext), getUnreachableBlock(), landingPad);
	enterUnreachable();
}

void EmitFunctionContext::delegate(DelegateImm imm)
{
	end(NoImm{});
}
