#include <cxxabi.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>
#include "RuntimePrivate.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/HashSet.h"
#include "WAVM/LLVMJIT/LLVMJIT.h"
#include "WAVM/Platform/Diagnostics.h"
#include "WAVM/Platform/RWMutex.h"
#include "WAVM/Platform/Signal.h"
#include "WAVM/Runtime/Intrinsics.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

using namespace WAVM;
using namespace WAVM::Runtime;

namespace WAVM { namespace Runtime {
	WAVM_DEFINE_INTRINSIC_MODULE(wavmIntrinsicsException)
}}

#define DEFINE_INTRINSIC_EXCEPTION_TYPE(name, ...)                                                 \
	ExceptionType* Runtime::ExceptionTypes::name = new ExceptionType(                              \
		nullptr, IR::TypeTuple({__VA_ARGS__}), "wavm." #name);
WAVM_ENUM_INTRINSIC_EXCEPTION_TYPES(DEFINE_INTRINSIC_EXCEPTION_TYPE)
#undef DEFINE_INTRINSIC_EXCEPTION_TYPE

std::string Runtime::asString(const InstructionSource& source)
{
	switch(source.type)
	{
	case InstructionSource::Type::unknown: return "<unknown>";
	case InstructionSource::Type::native: return "host!" + asString(source.native);
	case InstructionSource::Type::wasm:
		return source.wasm.function->mutableData->debugName + '+'
			   + std::to_string(source.wasm.instructionIndex);
	default: WAVM_UNREACHABLE();
	};
}

bool Runtime::getInstructionSourceByAddress(Uptr ip, InstructionSource& outSource)
{
	LLVMJIT::InstructionSource llvmjitSource;
	if(!LLVMJIT::getInstructionSourceByAddress(ip, llvmjitSource))
	{
		outSource.type = InstructionSource::Type::native;
		return Platform::getInstructionSourceByAddress(ip, outSource.native);
	}
	else
	{
		outSource.type = InstructionSource::Type::wasm;
		outSource.wasm.function = llvmjitSource.function;
		outSource.wasm.instructionIndex = llvmjitSource.instructionIndex;
		return true;
	}
}

// Returns a vector of strings, each element describing a frame of the call stack. If the frame is a
// JITed function, use the JIT's information about the function to describe it, otherwise fallback
// to whatever platform-specific symbol resolution is available.
std::vector<std::string> Runtime::describeCallStack(const Platform::CallStack& callStack)
{
	std::vector<std::string> frameDescriptions;
	HashSet<Uptr> describedIPs;
	Uptr frameIndex = 0;
	while(frameIndex < callStack.frames.size())
	{
		if(frameIndex + 1 < callStack.frames.size()
		   && describedIPs.contains(callStack.frames[frameIndex].ip)
		   && describedIPs.contains(callStack.frames[frameIndex + 1].ip))
		{
			Uptr numOmittedFrames = 2;
			while(frameIndex + numOmittedFrames < callStack.frames.size()
				  && describedIPs.contains(callStack.frames[frameIndex + numOmittedFrames].ip))
			{ ++numOmittedFrames; }

			frameDescriptions.emplace_back("<" + std::to_string(numOmittedFrames)
										   + " redundant frames omitted>");

			frameIndex += numOmittedFrames;
		}
		else
		{
			const Uptr frameIP = callStack.frames[frameIndex].ip;

			std::string frameDescription;
			InstructionSource source;
			if(!getInstructionSourceByAddress(frameIP, source))
			{ frameDescription = "<unknown function>"; }
			else
			{
				frameDescription = asString(source);
			}

			describedIPs.add(frameIP);
			frameDescriptions.push_back(frameDescription);

			++frameIndex;
		}
	}
	return frameDescriptions;
}

ExceptionType* Runtime::createExceptionType(Compartment* compartment,
											const IR::TypeTuple& sig,
											std::string&& debugName)
{
	auto exceptionType = new ExceptionType(compartment, sig, std::move(debugName));

	Platform::RWMutex::ExclusiveLock compartmentLock(compartment->mutex);
	exceptionType->id = compartment->exceptionTypes.add(UINTPTR_MAX, exceptionType);
	if(exceptionType->id == UINTPTR_MAX)
	{
		delete exceptionType;
		return nullptr;
	}

	return exceptionType;
}

ExceptionType* Runtime::cloneExceptionType(ExceptionType* exceptionType,
										   Compartment* newCompartment)
{
	auto newExceptionType = new ExceptionType(
		newCompartment, exceptionType->sig, std::string(exceptionType->debugName));
	newExceptionType->id = exceptionType->id;

	Platform::RWMutex::ExclusiveLock compartmentLock(newCompartment->mutex);
	newCompartment->exceptionTypes.insertOrFail(exceptionType->id, newExceptionType);
	return newExceptionType;
}

Runtime::ExceptionType::~ExceptionType()
{
	if(id != UINTPTR_MAX)
	{
		WAVM_ASSERT_RWMUTEX_IS_EXCLUSIVELY_LOCKED_BY_CURRENT_THREAD(compartment->mutex);
		compartment->exceptionTypes.removeOrFail(id);
	}
}

std::string Runtime::describeExceptionType(const ExceptionType* type)
{
	WAVM_ASSERT(type);
	return type->debugName;
}

IR::TypeTuple Runtime::getExceptionTypeParameters(const ExceptionType* type)
{
	return type->sig;
}

static std::atomic_int _exc_count;


Exception::Exception()
: typeId{}
, type{}
, isUserException{}
, arguments{}
, callStack{}
{
	++_exc_count;
	printf("%p: NIL exception (count=%d)\n", this, _exc_count.load());
}

Exception::Exception(ExceptionType* type,
					 const std::vector<IR::UntaggedValue>& arguments,
					 Platform::CallStack&& callStack)
: typeId(type->id)
, type(type)
, isUserException(type->compartment != nullptr)
, arguments(nullptr)
, callStack(std::move(callStack))
{
	++_exc_count;

	const IR::TypeTuple& params = type->sig;
printf("%p: USER exception type %ld nargs=%ld (count=%d)\n", this, typeId, params.size(), _exc_count.load());

	if (params.size())
	{
		auto p = new IR::UntaggedValue[params.size()];
		memcpy(p, arguments.data(), sizeof(IR::UntaggedValue) * params.size());
		this->arguments = p;
	}
}

Exception::Exception(ExceptionType* type,
					 const IR::UntaggedValue* arguments,
					 Platform::CallStack&& callStack)
: typeId(type->id)
, type(type)
, isUserException(type->compartment != nullptr)
, arguments(nullptr)
, callStack(std::move(callStack))
{
	++_exc_count;

	const IR::TypeTuple& params = type->sig;

printf("%p: create exception type %ld nargs=%ld (count=%d)\n", this, typeId, params.size(), _exc_count.load());
	if (params.size())
	{
		auto p = new IR::UntaggedValue[params.size()];
		memcpy(p, arguments, sizeof(IR::UntaggedValue) * params.size());
		this->arguments = p;
	}
}

Exception::~Exception()
{
	--_exc_count;
printf("%p: destroy exception type %ld args=%p (count=%d)\n", this, typeId, arguments, _exc_count.load());
	if (arguments)
	{
		delete[] arguments;
	}
}

ExceptionType* Runtime::getExceptionType(const Exception& exception) { return exception.type; }

IR::UntaggedValue Runtime::getExceptionArgument(const Exception& exception, Uptr argIndex)
{
	WAVM_ERROR_UNLESS(argIndex < exception.type->sig.size());
	return exception.arguments[argIndex];
}

const Platform::CallStack& Runtime::getExceptionCallStack(const Exception& exception)
{
	return exception.callStack;
}

std::string Runtime::describeException(const Exception& exception)
{
	std::string result = exception.type ? describeExceptionType(exception.type) : "<null exception>";
	if(exception.type == ExceptionTypes::outOfBoundsMemoryAccess)
	{
		Memory* memory = asMemoryNullable(exception.arguments[0].object);
		result += '(';
		result += memory ? memory->debugName : "<unknown memory>";
		result += '+';
		result += std::to_string(exception.arguments[1].u64);
		result += ')';
	}
	else if(exception.type == ExceptionTypes::outOfBoundsTableAccess
			|| exception.type == ExceptionTypes::uninitializedTableElement)
	{
		Table* table = asTableNullable(exception.arguments[0].object);
		result += '(';
		result += table ? table->debugName : "<unknown table>";
		result += '[';
		result += std::to_string(exception.arguments[1].u64);
		result += "])";
	}
	else if(exception.type == ExceptionTypes::indirectCallSignatureMismatch)
	{
		Function* function = exception.arguments[0].function;
		IR::FunctionType expectedSignature(
			IR::FunctionType::Encoding{Uptr(exception.arguments[1].u64)});
		result += '(';
		if(!function) { result += "<unknown function>"; }
		else
		{
			result += function->mutableData->debugName;
			result += " : ";
			result += asString(getFunctionType(function));
		}
		result += ", ";
		result += asString(expectedSignature);
		result += ')';
	}
	else
	{
		result += "typeId=" + std::to_string(exception.typeId) + " ";

		result += "sig=(";
		for(Uptr argumentIndex = 0; argumentIndex < exception.type->sig.size();
			++argumentIndex)
		{
			if(argumentIndex != 0) { result += ", "; }
			result += asString(IR::Value(exception.type->sig[argumentIndex],
										 exception.arguments[argumentIndex]));
		}
		result += ')';
	}
	std::vector<std::string> callStackDescription = describeCallStack(exception.callStack);
	result += "\nCall stack:\n";
	for(auto calledFunction : callStackDescription)
	{
		result += "  ";
		result += calledFunction.c_str();
		result += '\n';
	}
	return result;
}

[[noreturn]] void Runtime::throwException(ExceptionType* type,
										  const std::vector<IR::UntaggedValue>& arguments,
										  Platform::CallStack&& callStack)
{
	WAVM_ASSERT(type);
	WAVM_ASSERT(type->sig.size() == arguments.size());

	throw Exception(type, arguments, std::move(callStack));
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsicsException,
							   "throwException",
							   Uptr,
							   intrinsicCreateException,
							   Uptr exceptionTypeId,
							   Uptr argsBits,
							   U32 isUserException)
{
	printf("-----------------------------------------------\n");
#if 1
	ExceptionType* type;
	{
		Compartment* compartment = getCompartmentRuntimeData(contextRuntimeData)->compartment;
		Platform::RWMutex::ExclusiveLock compartmentLock(compartment->mutex);
		type = compartment->exceptionTypes[exceptionTypeId];
	}
	auto args = reinterpret_cast<const IR::UntaggedValue*>(Uptr(argsBits));

	throw Exception(type, args, Platform::captureCallStack(1));
#else
	throw 5;
#endif
}

static bool isRuntimeException(const Platform::Signal& signal)
{
	switch(signal.type)
	{
	case Platform::Signal::Type::accessViolation: {
		// If the access violation occured in a Memory or Table's reserved pages, it's a runtime
		// exception.
		Table* table = nullptr;
		Uptr tableIndex = 0;
		Memory* memory = nullptr;
		Uptr memoryAddress = 0;
		U8* badPointer = reinterpret_cast<U8*>(signal.accessViolation.address);
		return isAddressOwnedByTable(badPointer, table, tableIndex)
			   || isAddressOwnedByMemory(badPointer, memory, memoryAddress);
	}
	case Platform::Signal::Type::stackOverflow:
	case Platform::Signal::Type::intDivideByZeroOrOverflow: return true;

	case Platform::Signal::Type::invalid:
	default: WAVM_UNREACHABLE();
	}
}

[[noreturn]] static void translateSignalToRuntimeException(const Platform::Signal& signal,
														   Platform::CallStack&& callStack)
{
	switch(signal.type)
	{
	case Platform::Signal::Type::accessViolation: {
		// If the access violation occured in a Table's reserved pages, treat it as an undefined
		// table element runtime error.
		Table* table = nullptr;
		Uptr tableIndex = 0;
		Memory* memory = nullptr;
		Uptr memoryAddress = 0;
		U8* const badPointer = reinterpret_cast<U8*>(signal.accessViolation.address);
		if(isAddressOwnedByTable(badPointer, table, tableIndex))
		{
			throwException(ExceptionTypes::outOfBoundsTableAccess,
						   {table, U64(tableIndex)},
						   std::move(callStack));
		}
		// If the access violation occured in a Memory's reserved pages, treat it as an
		// out-of-bounds memory access.
		else if(isAddressOwnedByMemory(badPointer, memory, memoryAddress))
		{
			throwException(ExceptionTypes::outOfBoundsMemoryAccess,
						   {memory, U64(memoryAddress)},
						   std::move(callStack));
		}
		break;
	}
	case Platform::Signal::Type::stackOverflow:
		throwException(ExceptionTypes::stackOverflow, {}, std::move(callStack));
		break;
	case Platform::Signal::Type::intDivideByZeroOrOverflow:
		throwException(ExceptionTypes::integerDivideByZeroOrOverflow, {}, std::move(callStack));
		break;

	case Platform::Signal::Type::invalid:
	default:
		break;
	}
	WAVM_UNREACHABLE();
}

void Runtime::catchRuntimeExceptions(const std::function<void()>& thunk,
									 const std::function<void(const Exception&)>& catchThunk)
{
	try
	{
		unwindSignalsAsExceptions(thunk);
	}
	catch(const Exception& exception)
	{
		catchThunk(exception);
	}
}

void Runtime::unwindSignalsAsExceptions(const std::function<void()>& thunk)
{
	// Catch signals and translate them into runtime exceptions.
	struct UnwindContext
	{
		const std::function<void()>* thunk;
		Platform::Signal signal;
		Platform::CallStack callStack;
	} context;
	context.thunk = &thunk;
	if(Platform::catchSignals(
		   [](void* contextVoid) {
			   UnwindContext& context = *(UnwindContext*)contextVoid;
			   (*context.thunk)();
		   },
		   [](void* contextVoid, Platform::Signal signal, Platform::CallStack&& callStack) {
			   if(!isRuntimeException(signal)) { return false; }
			   else
			   {
				   UnwindContext& context = *(UnwindContext*)contextVoid;
				   context.signal = signal;
				   context.callStack = std::move(callStack);
				   return true;
			   }
		   },
		   &context))
	{
		translateSignalToRuntimeException(context.signal, std::move(context.callStack));
	}
}
