ADD_WAST_TESTS(
	NAME_PREFIX WebAssembly/exception-handling/
	SOURCES
		rethrow.wast
		tag.wast
		try_catch.wast
		try_delegate.wast
	WAVM_ARGS --test-cloning)
