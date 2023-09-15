#ifndef _WIN32
#include <cxxabi.h>
#include <unwind.h>
#endif

#include <unistd.h>

#include "WAVM/LLVMJIT/LLVMJIT.h"
#include <utility>
#include "LLVMJITPrivate.h"
#include "WAVM/IR/FeatureSpec.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/HashMap.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

namespace llvm {
	class Constant;
}

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;

namespace LLVMRuntimeSymbols {

	typedef uintptr_t _Unwind_Ptr;

	struct _Unwind_Exception;

	typedef unsigned _Unwind_Exception_Class __attribute__((__mode__(__DI__)));

	typedef enum
	{
		_URC_NO_REASON = 0,
		_URC_FOREIGN_EXCEPTION_CAUGHT = 1,
		_URC_FATAL_PHASE2_ERROR = 2,
		_URC_FATAL_PHASE1_ERROR = 3,
		_URC_NORMAL_STOP = 4,
		_URC_END_OF_STACK = 5,
		_URC_HANDLER_FOUND = 6,
		_URC_INSTALL_CONTEXT = 7,
		_URC_CONTINUE_UNWIND = 8
	} _Unwind_Reason_Code;


	typedef void (*_Unwind_Exception_Cleanup_Fn) (_Unwind_Reason_Code,
												 struct _Unwind_Exception *);

	typedef unsigned _Unwind_Word __attribute__((__mode__(__unwind_word__)));

	struct _Unwind_Exception
	{
		_Unwind_Exception_Class exception_class;
		_Unwind_Exception_Cleanup_Fn exception_cleanup;

#if !defined (__USING_SJLJ_EXCEPTIONS__) && defined (__SEH__)
		_Unwind_Word private_[6];
#else
		_Unwind_Word private_1;
		_Unwind_Word private_2;
#endif

		/* @@@ The IA-64 ABI says that this structure must be double-word aligned.
		   Taking that literally does not make much sense generically.  Instead we
		   provide the maximum alignment required by any type for the machine.  */
	} __attribute__((__aligned__));

	struct __cxa_exception
	{
		// Manage the exception object itself.
		std::type_info *exceptionType;
		void (_GLIBCXX_CDTOR_CALLABI *exceptionDestructor)(void *);

			   // The C++ standard has entertaining rules wrt calling set_terminate
			   // and set_unexpected in the middle of the exception cleanup process.
		std::terminate_handler unexpectedHandler;
		std::terminate_handler terminateHandler;

			   // The caught exception stack threads through here.
		__cxa_exception *nextException;

			   // How many nested handlers have caught this exception.  A negated
			   // value is a signal that this object has been rethrown.
		int handlerCount;

#ifdef __ARM_EABI_UNWINDER__
		// Stack of exceptions in cleanups.
		__cxa_exception* nextPropagatingException;

			   // The number of active cleanup handlers for this exception.
		int propagationCount;
#else
		// Cache parsed handler data from the personality routine Phase 1
		// for Phase 2 and __cxa_call_unexpected.
		int handlerSwitchValue;
		const unsigned char *actionRecord;
		const unsigned char *languageSpecificData;
		_Unwind_Ptr catchTemp;
		void *adjustedPtr;
#endif

			   // The generic exception header.  Must be last.
		_Unwind_Exception unwindHeader;
	};

	struct __cxa_eh_globals
	{
		__cxa_exception *caughtExceptions;
		unsigned int uncaughtExceptions;
	};

	void print_caught(__cxa_exception* ch)
	{
		printf("  caught exceptions:\n");
		for (; ch; ch = ch->nextException)
		{
			printf("    %p handlers=%d\n", ch, ch->handlerCount);
		}
	}

	void* cxa_get_exception_ptr(void* p)
	{
		auto eh_globals = reinterpret_cast<__cxa_eh_globals*>(__cxxabiv1::__cxa_get_globals_fast());
		printf("cxa_get_exception_ptr %p\n", eh_globals);
		auto rv = __cxxabiv1::__cxa_get_exception_ptr(p);
		return rv;
	}
	void* cxa_begin_catch(void* p)
	{
		auto eh_globals = reinterpret_cast<__cxa_eh_globals*>(__cxxabiv1::__cxa_get_globals_fast());
		printf("cxa_begin_catch %p\n", eh_globals);
		print_caught(eh_globals->caughtExceptions);
		auto rv = __cxxabiv1::__cxa_begin_catch(p);
		printf("/cxa_begin_catch %p\n", eh_globals);
		print_caught(eh_globals->caughtExceptions);
		return rv;
	}
	void cxa_end_catch()
	{
		auto eh_globals = reinterpret_cast<__cxa_eh_globals*>(__cxxabiv1::__cxa_get_globals_fast());
		printf("cxa_end_catch %p\n", eh_globals);
		print_caught(eh_globals->caughtExceptions);
		__cxxabiv1::__cxa_end_catch();
		printf("/cxa_end_catch %p\n", eh_globals);
		print_caught(eh_globals->caughtExceptions);
	}
	void cxa_rethrow()
	{
		auto eh_globals = reinterpret_cast<__cxa_eh_globals*>(__cxxabiv1::__cxa_get_globals_fast());
		printf("cxa_rethrow %p\n", eh_globals);
		print_caught(eh_globals->caughtExceptions);
		__cxxabiv1::__cxa_rethrow();
	}
#ifdef _WIN32
	// the LLVM X86 code generator calls __chkstk when allocating more than 4KB of stack space
	extern "C" void __chkstk();
	extern "C" void __CxxFrameHandler3();
#else
#if defined(__APPLE__)
	// LLVM's memset intrinsic lowers to calling __bzero on MacOS when writing a constant zero.
	extern "C" void __bzero();
#endif
#if defined(__i386__) || defined(__x86_64__)
	extern "C" void wavm_probe_stack();
#endif
	extern "C" int __gxx_personality_v0();
#endif

	static HashMap<std::string, void*> map = {
		{"memmove", (void*)&memmove},
		{"memset", (void*)&memset},
#ifdef _WIN32
		{"__chkstk", (void*)&__chkstk},
		{"__CxxFrameHandler3", (void*)&__CxxFrameHandler3},
#else
#if defined(__APPLE__)
		{"__bzero", (void*)&__bzero},
#endif
#if defined(__i386__) || defined(__x86_64__)
		{"wavm_probe_stack", (void*)&wavm_probe_stack},
#endif
		{"__gxx_personality_v0", (void*)&__gxx_personality_v0},
		{"__cxa_rethrow", (void*)&cxa_rethrow},
		{"__cxa_get_exception_ptr", (void*)&cxa_get_exception_ptr},
		{"__cxa_begin_catch", (void*)&cxa_begin_catch},
		{"__cxa_end_catch", (void*)&cxa_end_catch},
		{"_Unwind_Resume", (void*)&_Unwind_Resume},
#endif
	};
}

llvm::JITEvaluatedSymbol LLVMJIT::resolveJITImport(llvm::StringRef name)
{
	// Allow some intrinsics used by LLVM
	void** symbolValue = LLVMRuntimeSymbols::map.get(name.str());
	if(!symbolValue)
	{
		Errors::fatalf("LLVM generated code references unknown external symbol: %s",
					   name.str().c_str());
	}

	return llvm::JITEvaluatedSymbol(reinterpret_cast<Uptr>(*symbolValue),
									llvm::JITSymbolFlags::None);
}

static bool globalInitLLVM()
{
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmPrinters();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllDisassemblers();
	llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
	return true;
}

static void globalInitLLVMOnce()
{
	static bool isLLVMInitialized = globalInitLLVM();
	WAVM_ASSERT(isLLVMInitialized);
}

LLVMContext::LLVMContext()
{
	globalInitLLVMOnce();

	i8Type = llvm::Type::getInt8Ty(*this);
	i16Type = llvm::Type::getInt16Ty(*this);
	i32Type = llvm::Type::getInt32Ty(*this);
	i64Type = llvm::Type::getInt64Ty(*this);
	f32Type = llvm::Type::getFloatTy(*this);
	f64Type = llvm::Type::getDoubleTy(*this);
	i8PtrType = i8Type->getPointerTo();

	externrefType = llvm::StructType::create("Object", i8Type)->getPointerTo();

	i8x8Type = FixedVectorType::get(i8Type, 8);
	i16x4Type = FixedVectorType::get(i16Type, 4);
	i32x2Type = FixedVectorType::get(i32Type, 2);
	i64x1Type = FixedVectorType::get(i64Type, 1);
	f32x2Type = FixedVectorType::get(f32Type, 2);
	f64x1Type = FixedVectorType::get(f64Type, 1);

	i8x16Type = FixedVectorType::get(i8Type, 16);
	i16x8Type = FixedVectorType::get(i16Type, 8);
	i32x4Type = FixedVectorType::get(i32Type, 4);
	i64x2Type = FixedVectorType::get(i64Type, 2);
	f32x4Type = FixedVectorType::get(f32Type, 4);
	f64x2Type = FixedVectorType::get(f64Type, 2);

	i8x32Type = FixedVectorType::get(i8Type, 32);
	i16x16Type = FixedVectorType::get(i16Type, 16);
	i32x8Type = FixedVectorType::get(i32Type, 8);
	i64x4Type = FixedVectorType::get(i64Type, 4);

	i8x48Type = FixedVectorType::get(i8Type, 48);
	i16x24Type = FixedVectorType::get(i16Type, 24);
	i32x12Type = FixedVectorType::get(i32Type, 12);
	i64x6Type = FixedVectorType::get(i64Type, 6);

	i8x64Type = FixedVectorType::get(i8Type, 64);
	i16x32Type = FixedVectorType::get(i16Type, 32);
	i32x16Type = FixedVectorType::get(i32Type, 16);
	i64x8Type = FixedVectorType::get(i64Type, 8);

	valueTypes[(Uptr)ValueType::none] = valueTypes[(Uptr)ValueType::any] = nullptr;
	valueTypes[(Uptr)ValueType::i32] = i32Type;
	valueTypes[(Uptr)ValueType::i64] = i64Type;
	valueTypes[(Uptr)ValueType::f32] = f32Type;
	valueTypes[(Uptr)ValueType::f64] = f64Type;
	valueTypes[(Uptr)ValueType::v128] = i64x2Type;
	valueTypes[(Uptr)ValueType::externref] = externrefType;
	valueTypes[(Uptr)ValueType::funcref] = externrefType;

	// Create zero constants of each type.
	typedZeroConstants[(Uptr)ValueType::none] = nullptr;
	typedZeroConstants[(Uptr)ValueType::any] = nullptr;
	typedZeroConstants[(Uptr)ValueType::i32] = emitLiteral(*this, (U32)0);
	typedZeroConstants[(Uptr)ValueType::i64] = emitLiteral(*this, (U64)0);
	typedZeroConstants[(Uptr)ValueType::f32] = emitLiteral(*this, (F32)0.0f);
	typedZeroConstants[(Uptr)ValueType::f64] = emitLiteral(*this, (F64)0.0);
	typedZeroConstants[(Uptr)ValueType::v128] = emitLiteral(*this, V128());
	typedZeroConstants[(Uptr)ValueType::externref] = typedZeroConstants[(Uptr)ValueType::funcref]
		= llvm::Constant::getNullValue(externrefType);
}

TargetSpec LLVMJIT::getHostTargetSpec()
{
	TargetSpec result;
	result.triple = llvm::sys::getProcessTriple();
	result.cpu = std::string(llvm::sys::getHostCPUName());
	return result;
}

std::unique_ptr<llvm::TargetMachine> LLVMJIT::getTargetMachine(const TargetSpec& targetSpec)
{
	globalInitLLVMOnce();

	llvm::Triple triple(targetSpec.triple);
	llvm::SmallVector<std::string, 1> targetAttributes;

#if LLVM_VERSION_MAJOR < 10
	if(triple.getArch() == llvm::Triple::x86 || triple.getArch() == llvm::Triple::x86_64)
	{
		// Disable AVX-512 on X86 targets to workaround a LLVM backend bug:
		// https://bugs.llvm.org/show_bug.cgi?id=43750
		targetAttributes.push_back("-avx512f");
	}
#endif

	return std::unique_ptr<llvm::TargetMachine>(
		llvm::EngineBuilder().selectTarget(triple, "", targetSpec.cpu, targetAttributes));
}

TargetValidationResult LLVMJIT::validateTargetMachine(
	const std::unique_ptr<llvm::TargetMachine>& targetMachine,
	const FeatureSpec& featureSpec)
{
	const llvm::Triple::ArchType targetArch = targetMachine->getTargetTriple().getArch();
	if(targetArch == llvm::Triple::x86_64)
	{
		// If the SIMD feature is enabled, then require the SSE4.1 CPU feature.
		if(featureSpec.simd && !targetMachine->getMCSubtargetInfo()->checkFeatures("+sse4.1"))
		{ return TargetValidationResult::x86CPUDoesNotSupportSSE41; }

		return TargetValidationResult::valid;
	}
	else if(targetArch == llvm::Triple::aarch64)
	{
		if(featureSpec.simd && !targetMachine->getMCSubtargetInfo()->checkFeatures("+neon"))
		{ return TargetValidationResult::wavmDoesNotSupportSIMDOnArch; }

		return TargetValidationResult::valid;
	}
	else
	{
		if(featureSpec.simd) { return TargetValidationResult::wavmDoesNotSupportSIMDOnArch; }
		if(featureSpec.memory64) { return TargetValidationResult::memory64Requires64bitTarget; }
		if(featureSpec.table64) { return TargetValidationResult::table64Requires64bitTarget; }
		return TargetValidationResult::unsupportedArchitecture;
	}
}

TargetValidationResult LLVMJIT::validateTarget(const TargetSpec& targetSpec,
											   const IR::FeatureSpec& featureSpec)
{
	std::unique_ptr<llvm::TargetMachine> targetMachine = getTargetMachine(targetSpec);
	if(!targetMachine) { return TargetValidationResult::invalidTargetSpec; }
	return validateTargetMachine(targetMachine, featureSpec);
}

Version LLVMJIT::getVersion()
{
	return Version{LLVM_VERSION_MAJOR, LLVM_VERSION_MINOR, LLVM_VERSION_PATCH, 5};
}
