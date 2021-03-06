add_library(slang STATIC
	binding/BindContext.cpp
	binding/ConstantValue.cpp
	binding/EvalContext.cpp
	binding/Expressions.cpp
	binding/Expressions_eval.cpp
	binding/Statements.cpp

	compilation/BuiltInSubroutines.cpp
	compilation/Compilation.cpp
	compilation/SemanticModel.cpp

	diagnostics/Diagnostics.cpp
	diagnostics/DiagnosticWriter.cpp

	numeric/SVInt.cpp
	numeric/Time.cpp
	numeric/VectorBuilder.cpp

	parsing/Lexer.cpp
	parsing/LexerFacts.cpp
	parsing/Parser.cpp
	parsing/Parser_expressions.cpp
	parsing/Parser_statements.cpp
	parsing/ParserBase.cpp
	parsing/Preprocessor.cpp
	parsing/Token.cpp

	symbols/DeclaredType.cpp
	symbols/HierarchySymbols.cpp
	symbols/MemberSymbols.cpp
	symbols/Scope.cpp
	symbols/SemanticFacts.cpp
	symbols/StatementBodiedScope.cpp
	symbols/Symbol.cpp
	symbols/TypePrinter.cpp
	symbols/TypeSymbols.cpp

	syntax/AllSyntax.cpp
	syntax/SyntaxFacts.cpp
	syntax/SyntaxNode.cpp
	syntax/SyntaxPrinter.cpp
	syntax/SyntaxVisitor.cpp

	text/SourceManager.cpp

	util/BumpAllocator.cpp
	util/Hash.cpp
	util/Util.cpp

	../external/fmt/format.cc
	../external/fmt/posix.cc
)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	# Warnings as errors
	target_compile_options(slang PRIVATE /WX)
else()
	# Link against C++17 filesystem
	target_link_libraries(slang PUBLIC stdc++fs)
	target_compile_options(slang PRIVATE
		-Wall
		-Wextra
		-Werror
	)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(slang PRIVATE
		-Warray-bounds-pointer-arithmetic
		-Wassign-enum
		-Wbad-function-cast
		-Wcast-qual
		-Wcomma
		-Wduplicate-enum
		-Wduplicate-method-arg
		-Wimplicit-fallthrough
		-Wrange-loop-analysis
		-Wpedantic
		-Wconversion
		-Wshadow
		-Wno-missing-braces
	)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	target_compile_options(slang PRIVATE
		-Wunused-value
		-Wformat-security
		-Wnull-dereference
		-Wimplicit-fallthrough=5
		-Walloc-zero
		-Wlogical-op
		-Wlogical-not-parentheses
		-Wvla
		-Wnoexcept
		-Wduplicated-cond
		-Wtype-limits
	)
endif()

target_link_libraries(slang PUBLIC CONAN_PKG::jsonformoderncpp)

target_include_directories(slang PUBLIC ../include/)
target_include_directories(slang SYSTEM PUBLIC ../external/)
