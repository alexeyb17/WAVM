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

static llvm::Function* getCXAAllocateExceptionFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaAllocateExceptionFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvmContext.i8PtrType, {moduleContext.iptrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_allocate_exception",
			moduleContext.llvmModule);
	}
	return rv;
}

static llvm::Function* getCXAThrowFunction(EmitModuleContext& moduleContext)
{
	auto& rv = moduleContext.cxaThrowFunction;
	if(!rv)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		rv = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext),
									{moduleContext.iptrType, llvmContext.i8PtrType, moduleContext.iptrType},
									false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_throw",
			moduleContext.llvmModule);
	}
	return rv;
}

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

void EmitFunctionContext::endTryCatch()
{
	ControlContext& currentContext = controlStack.back();
	WAVM_ASSERT(currentContext.type == ControlContext::Type::catch_
				|| currentContext.type == ControlContext::Type::catch_all);

	CatchContext& catchContext = currentContext.catchContext;

	exitCatch();

	// If an end instruction terminates a sequence of catch clauses, terminate the chain of
	// handler type ID tests by rethrowing the exception if its type ID didn't match any of the
	// handlers.
	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();

	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	emitCallOrInvoke(getCXARethrowFunction(moduleContext), {}, FunctionType({}, {}, CallingConvention::c));
	irBuilder.CreateUnreachable();

	irBuilder.SetInsertPoint(savedInsertionPoint);
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

llvm::BasicBlock* EmitFunctionContext::getInnermostUnwindToBlock()
{
	for (auto it = controlStack.rbegin(); it != controlStack.rend(); ++it)
	{
		if (it->type == ControlContext::Type::try_)
		{
			return it->catchContext.unwindToBlock;
		}
	}
	return nullptr;
}

void EmitFunctionContext::try_(ControlStructureImm imm)
{
	auto originalInsertBlock = irBuilder.GetInsertBlock();

	CatchContext catchContext;
	int delegateImm = lookupClosingDelegate();
	if (delegateImm >= 0)
	{
		auto offset = static_cast<Uptr>(delegateImm);
		WAVM_ASSERT(offset < controlStack.size());
		for (; offset < controlStack.size(); ++offset)
		{
			const auto& controlContext = controlStack[controlStack.size() - 1 - offset];
			if (controlContext.type == ControlContext::Type::try_)
			{
				// found target `try` block, use its CatchContext
				catchContext = controlContext.catchContext;
				break;
			}
		}
		// if `try` isn't found CatchContext remains zero-initialized which
		// means rethrowing to caller function
	}
	else
	{
		// try/catch - generate new CatchContext
		if (moduleContext.useWindowsSEH)
		{
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
		}
		else
		{
			// Create a BasicBlock with a LandingPad instruction to use as the unwind target.
			auto landingPadBlock = llvm::BasicBlock::Create(llvmContext, "landingPad", function);
			irBuilder.SetInsertPoint(landingPadBlock);
			auto landingPadInst = irBuilder.CreateLandingPad(
				llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
			landingPadInst->addClause(moduleContext.runtimeExceptionTypeInfo);

			// Call __cxa_begin_catch to get the exception pointer.
			auto exceptionPointer
				= irBuilder.CreateCall(getCXABeginCatchFunction(moduleContext),
									   {irBuilder.CreateExtractValue(landingPadInst, {0})});

			// Load the exception type ID.
			auto exceptionTypeId = loadFromUntypedPointer(
				createInBoundsGEP(
					llvmContext.i8Type,
					exceptionPointer,
					{emitLiteralIptr(offsetof(Exception, typeId), moduleContext.iptrType)}),
				moduleContext.iptrType);

			catchContext.unwindToBlock = landingPadBlock;
			catchContext.landingPadInst = landingPadInst;
			catchContext.exceptionPointer = exceptionPointer;
			catchContext.nextHandlerBlock = landingPadBlock;
			catchContext.exceptionTypeId = exceptionTypeId;
		}
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

	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	auto isExceptionType = irBuilder.CreateICmpEQ(catchContext.exceptionTypeId, catchTypeId);

	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);

	// Push the exception arguments on the stack.
	for(Uptr argumentIndex = 0; argumentIndex < exceptionParams.size(); ++argumentIndex)
	{
		const ValueType parameters = exceptionParams[argumentIndex];
		const Uptr argOffset
			= offsetof(Exception, arguments)
			  + (exceptionParams.size() - argumentIndex - 1) * sizeof(Exception::arguments[0]);
		auto argument = loadFromUntypedPointer(
			createInBoundsGEP(llvmContext.i8Type, catchContext.exceptionPointer,
							  {emitLiteral(llvmContext, argOffset)}),
			asLLVMType(llvmContext, parameters),
			sizeof(Exception::arguments[0]));
		push(argument);
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
	}
	else
	{
		exitCatch();
		branchToEndOfControlContext();
		controlContext.type = ControlContext::Type::catch_all;
		controlContext.isReachable = true;
	}

	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
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

	llvm::Value* exceptionSize = emitLiteral(llvmContext,
											 I64(offsetof(Exception, arguments) + (numArgs == 0 ? 1u : numArgs) * sizeof(IR::UntaggedValue)));
	auto exceptionMem
		= irBuilder.CreateCall(getCXAAllocateExceptionFunction(moduleContext), {exceptionSize});
	llvm::Value* exceptionMemAsInt
		= irBuilder.CreatePtrToInt(exceptionMem, moduleContext.iptrType);

	llvm::Value* exceptionPointer = emitRuntimeIntrinsic(
		"createException",
		FunctionType(
			TypeTuple{moduleContext.iptrValueType},
			TypeTuple{moduleContext.iptrValueType, moduleContext.iptrValueType, moduleContext.iptrValueType, ValueType::i32},
			IR::CallingConvention::intrinsic),
		{exceptionMemAsInt, exceptionTypeId, argsPointerAsInt, emitLiteral(llvmContext, I32(1))})[0];

	emitCallOrInvoke(getCXAThrowFunction(moduleContext),
					 {exceptionPointer,
					  irBuilder.CreatePtrToInt(moduleContext.runtimeExceptionTypeInfo, moduleContext.iptrType),
					  emitLiteralIptr(0, moduleContext.iptrType)},
					 FunctionType(
						 TypeTuple{},
						 TypeTuple{moduleContext.iptrValueType, moduleContext.iptrValueType, moduleContext.iptrValueType},
						 IR::CallingConvention::c));

	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::rethrow(RethrowImm imm)
{
	WAVM_ASSERT(imm.catchDepth < controlStack.size());
	auto& controlContext = controlStack[controlStack.size() - imm.catchDepth - 1];
	WAVM_ASSERT(controlContext.type == ControlContext::Type::catch_
				|| controlContext.type == ControlContext::Type::catch_all);

	// End all innermost catch blocks before rethrowing exception pointed by `rethrow imm`.
	for (Uptr depth = 0; depth != imm.catchDepth; ++depth)
	{
		auto& innerContext = controlStack[controlStack.size() - depth - 1];
		if (innerContext.type == ControlContext::Type::catch_
		   || innerContext.type == ControlContext::Type::catch_all)
		{
			irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
		}
	}
	// Rethrow exception from target catch.
	emitCallOrInvoke(getCXARethrowFunction(moduleContext), {}, FunctionType({}, {}, CallingConvention::c));

	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::delegate(DelegateImm imm)
{
	end(NoImm{});
}

int EmitFunctionContext::lookupClosingDelegate()
{
	struct Visitor
	{
		typedef void Result;

#define VISIT_OP(opcode, name, nameString, Imm, ...)                                               \
		void name(Imm imm) {}
		WAVM_ENUM_NONCONTROL_OPERATORS(VISIT_OP)
		VISIT_OP(_, unknown, "unknown", Opcode)
#undef VISIT_OP

		void block(ControlStructureImm) { ++controlDepth; }
		void loop(ControlStructureImm) { ++controlDepth; }
		void if_(ControlStructureImm) { ++controlDepth; }

		void else_(NoImm imm) {}
		void end(NoImm imm) { --controlDepth; }

		void try_(ControlStructureImm imm) { ++controlDepth; }
		void catch_(ExceptionTypeImm imm)
		{
			if (controlDepth == 1) { delegateDepth = -1; }
		}
		void catch_all(NoImm imm)
		{
			if (controlDepth == 1) { delegateDepth = -1; }
		}
		void delegate(DelegateImm imm)
		{
			if (--controlDepth == 0) { delegateDepth = static_cast<int>(imm.delegateDepth); }
		}

		Uptr controlDepth = 1;
		int delegateDepth = 0;
	};

	auto aheadDecoder = decoder;
	Visitor v;
	while (aheadDecoder)
	{
		aheadDecoder.decodeOp(v);
		if (v.controlDepth == 0)
		{
			return v.delegateDepth;
		}
	}
	// closing instruction is not found, treat as rethrow to upper level
	return 0;
}
