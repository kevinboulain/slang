// slang-cpp-compiler.cpp
#include "slang-cpp-compiler.h"

#include "slang-common.h"
#include "../../slang-com-helper.h"
#include "slang-string-util.h"

#include "slang-io.h"
#include "slang-shared-library.h"

#if SLANG_VC
#   include "windows/slang-win-visual-studio-util.h"
#endif

namespace Slang
{

/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! GenericCPPCompiler !!!!!!!!!!!!!!!!!!!!!!*/

SlangResult GenericCPPCompiler::compile(const CompileOptions& options, ExecuteResult& outResult)
{
    // Copy the command line options
    CommandLine cmdLine(m_cmdLine);

    // Append command line args to the end of cmdLine using the target specific function for the specified options
    m_func(options, cmdLine);

#if 0
    // Test
    {
        String line = ProcessUtil::getCommandLineString(cmdLine);
        printf("%s", line.getBuffer());
    }
#endif

    SlangResult res = ProcessUtil::execute(cmdLine, outResult);

#if 0
    {
        printf("stdout=\"%s\"\nstderr=\"%s\"\nret=%d\n", outResult.standardOutput.getBuffer(), outResult.standardError.getBuffer(), int(outResult.resultCode));
    }
#endif

    return res;
}

/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! CPPCompilerUtil !!!!!!!!!!!!!!!!!!!!!!*/

static bool _isDigit(char c)
{
    return c >= '0' && c <= '9';
}

static bool _isWhiteSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* static */SlangResult CPPCompilerUtil::parseGCCFamilyVersion(const UnownedStringSlice& text, const UnownedStringSlice& prefix, CPPCompiler::Desc& outDesc)
{
    List<UnownedStringSlice> lines;
    StringUtil::calcLines(text, lines);

    for (auto line : lines)
    {
        // TODO(JS): Ugh - having to turn into a string to do this test isn't great.
        if (String(line).startsWith(prefix))
        {
            UnownedStringSlice versionSlice(line.begin() + prefix.size(), line.end());

            List<Int> digits;

            const char* cur = versionSlice.begin();
            const char* end = versionSlice.end();

            // Consume white space
            while (cur < end && _isWhiteSpace(*cur)) cur++;

            // Version is in format 0.0.0 
            while (true)
            {
                Int value = 0;
                const char* start = cur;
                while (cur < end && _isDigit(*cur))
                {
                    value = value * 10 + (*cur - '0');
                    cur++;
                }

                if (cur <= start)
                {
                    break;
                }

                digits.add(value);

                if (cur < end && *cur == '.')
                {
                    cur++;
                }
            }

            if (digits.getCount() < 2)
            {
                return SLANG_FAIL;
            }

            outDesc.majorVersion = digits[0];
            outDesc.minorVersion = digits[1];
            return SLANG_OK;
        }
    }

    return SLANG_FAIL;
}

SlangResult CPPCompilerUtil::calcGCCFamilyVersion(const String& exeName, CPPCompiler::Desc& outDesc)
{
    CommandLine cmdLine;
    cmdLine.setExecutableFilename(exeName);
    cmdLine.addArg("-v");

    ExecuteResult exeRes;
    SLANG_RETURN_ON_FAIL(ProcessUtil::execute(cmdLine, exeRes));

    const UnownedStringSlice prefixes[] =
    {
        UnownedStringSlice::fromLiteral("clang version"),
        UnownedStringSlice::fromLiteral("gcc version"),
        UnownedStringSlice::fromLiteral("Apple LLVM version"),
    };
    const CPPCompiler::Type types[] =
    {
        CPPCompiler::Type::Clang,
        CPPCompiler::Type::GCC,
        CPPCompiler::Type::Clang,
    };

    SLANG_COMPILE_TIME_ASSERT(SLANG_COUNT_OF(prefixes) == SLANG_COUNT_OF(types));

    for (Index i = 0; i < SLANG_COUNT_OF(prefixes); ++i)
    {
        // Set the type
        outDesc.type = types[i];
        // Extract the version
        if (SLANG_SUCCEEDED(parseGCCFamilyVersion(exeRes.standardError.getUnownedSlice(), prefixes[i], outDesc)))
        {
            return SLANG_OK;
        }
    }
    return SLANG_FAIL;
}

/* static */void CPPCompilerUtil::calcVisualStudioArgs(const CompileOptions& options, CommandLine& cmdLine)
{
    // https://docs.microsoft.com/en-us/cpp/build/reference/compiler-options-listed-alphabetically?view=vs-2019

    cmdLine.addArg("/nologo");
    // Generate complete debugging information
    cmdLine.addArg("/Zi");
    // Display full path of source files in diagnostics
    cmdLine.addArg("/FC");

    if (options.flags & CompileOptions::Flag::EnableExceptionHandling)
    {
        if (options.sourceType == SourceType::CPP)
        {
            // https://docs.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model?view=vs-2019
            // Assumes c functions cannot throw
            cmdLine.addArg("/EHsc");
        }
    }

    switch (options.optimizationLevel)
    {
        case OptimizationLevel::Debug:
        {
            // No optimization
            cmdLine.addArg("/Od");

            cmdLine.addArg("/MDd");
            break;
        }
        case OptimizationLevel::Normal:
        {
            cmdLine.addArg("/O2");
            // Multithreaded DLL
            cmdLine.addArg("/MD");
            break;
        }
        default: break;
    }

    // /Fd - followed by name of the pdb file
    if (options.debugInfoType != DebugInfoType::None)
    {
        cmdLine.addPrefixPathArg("/Fd", options.modulePath, ".pdb");
    }

    switch (options.targetType)
    {
        case TargetType::SharedLibrary:
        {
            // Create dynamic link library
            if (options.optimizationLevel == OptimizationLevel::Debug)
            {
                cmdLine.addArg("/LDd");
            }
            else
            {
                cmdLine.addArg("/LD");
            }

            cmdLine.addPrefixPathArg("/Fe", options.modulePath, ".dll");
            break;
        }
        case TargetType::Executable:
        {
            cmdLine.addPrefixPathArg("/Fe", options.modulePath, ".exe");
            break;
        }
        default: break;
    }

    // Object file specify it's location - needed if we are out
    cmdLine.addPrefixPathArg("/Fo", options.modulePath, ".obj");

    // Add defines
    for (const auto& define : options.defines)
    {
        StringBuilder builder;
        builder << define.nameWithSig;
        if (define.value.getLength())
        {
            builder << "=" << define.value;
        }

        cmdLine.addArg(builder);
    }

    // Add includes
    for (const auto& include : options.includePaths)
    {
        cmdLine.addArg("/I");
        cmdLine.addArg(include);
    }

    // https://docs.microsoft.com/en-us/cpp/build/reference/eh-exception-handling-model?view=vs-2019
    // /Eha - Specifies the model of exception handling. (a, s, c, r are options)

    // Files to compile
    for (const auto& sourceFile : options.sourceFiles)
    {
        cmdLine.addArg(sourceFile);
    }

    // Link options (parameters past /link go to linker)
    cmdLine.addArg("/link");

    for (const auto& libPath : options.libraryPaths)
    {
        // Note that any escaping of the path is handled in the ProcessUtil::
        cmdLine.addPrefixPathArg("/LIBPATH:", libPath);
    }
}

/* static */void CPPCompilerUtil::calcGCCFamilyArgs(const CompileOptions& options, CommandLine& cmdLine)
{
    cmdLine.addArg("-fvisibility=hidden");
    // Use shared libraries
    //cmdLine.addArg("-shared");

    switch (options.optimizationLevel)
    {
        case OptimizationLevel::Debug:
        {
            // No optimization
            cmdLine.addArg("-O0");
            break;
        }
        case OptimizationLevel::Normal:
        {
            cmdLine.addArg("-Os");
            break;
        }
        default: break;
    }

    if (options.debugInfoType != DebugInfoType::None)
    {
        cmdLine.addArg("-g");
    }

    switch (options.targetType)
    {
        case TargetType::SharedLibrary:
        {
            // Shared library
            cmdLine.addArg("-shared");
            // Position independent
            cmdLine.addArg("-fPIC");

            String sharedLibraryPath = SharedLibrary::calcPlatformPath(options.modulePath.getUnownedSlice());

            cmdLine.addArg("-o");
            cmdLine.addArg(sharedLibraryPath);
            break;
        }
        case TargetType::Executable:
        {
            cmdLine.addArg("-o");

            StringBuilder builder;
            builder << options.modulePath;
            builder << ProcessUtil::getExecutableSuffix();

            cmdLine.addArg(options.modulePath);
            break;
        }
        case TargetType::Object:
        {
            // Don't link, just produce object file
            cmdLine.addArg("-c");
            break;
        }
        default: break;
    }

    // Add defines
    for (const auto& define : options.defines)
    {
        StringBuilder builder;
        builder << define.nameWithSig;
        if (define.value.getLength())
        {
            builder << "=" << define.value;
        }

        cmdLine.addArg(builder);
    }

    // Add includes
    for (const auto& include : options.includePaths)
    {
        cmdLine.addArg("-I");
        cmdLine.addArg(include);
    }

    // Link options
    if (0)
    {
        StringBuilder linkOptions;
        linkOptions << "Wl,";
        cmdLine.addArg(linkOptions);
    }

    // Files to compile
    for (const auto& sourceFile : options.sourceFiles)
    {
        cmdLine.addArg(sourceFile);
    }

    for (const auto& libPath : options.libraryPaths)
    {
        // Note that any escaping of the path is handled in the ProcessUtil::
        cmdLine.addArg("-L");
        cmdLine.addArg(libPath);
        cmdLine.addArg("-F");
        cmdLine.addArg(libPath);
    }

    if (options.sourceType == SourceType::CPP)
    {
        // Make STD libs available
        cmdLine.addArg("-lstdc++");
    }
}

static CPPCompiler::Desc _calcCompiledWithDesc()
{
    CPPCompiler::Desc desc = {};

#if SLANG_VC
    desc = WinVisualStudioUtil::getDesc(WinVisualStudioUtil::getCompiledVersion());
#elif SLANG_CLANG
    desc.type = CPPCompiler::Type::Clang;
    desc.majorVersion = Int(__clang_major__);
    desc.minorVersion = Int(__clang_minor__);
#elif SLANG_SNC
    desc.type = CPPCompiler::Type::SNC;
#elif SLANG_GHS
    desc.type = CPPCompiler::Type::GHS;
#elif SLANG_GCC
    desc.type = CPPCompiler::Type::GCC;
    desc.majorVersion = Int(__GNUC__);
    desc.minorVersion = Int(__GNUC_MINOR__);
#else
    desc.type = CPPCompiler::Type::Unknown;
#endif

    return desc;
}

const CPPCompiler::Desc& CPPCompilerUtil::getCompiledWithDesc()
{
    static CPPCompiler::Desc s_desc = _calcCompiledWithDesc();
    return s_desc;
}

/* static */CPPCompiler* CPPCompilerUtil::findCompiler(const CPPCompilerSet* set, MatchType matchType, const CPPCompiler::Desc& desc)
{
    List<CPPCompiler*> compilers;
    set->getCompilers(compilers);
    return findCompiler(compilers, matchType, desc);
}

/* static */CPPCompiler* CPPCompilerUtil::findCompiler(const List<CPPCompiler*>& compilers, MatchType matchType, const CPPCompiler::Desc& desc)
{
    Int bestIndex = -1;

    const CPPCompiler::Type type = desc.type;

    Int maxVersionValue = 0;
    Int minVersionDiff = 0x7fffffff;

    const auto descVersionValue = desc.getVersionValue();

    for (Index i = 0; i < compilers.getCount(); ++i)
    {
        CPPCompiler* compiler = compilers[i];
        auto compilerDesc = compiler->getDesc();

        if (type == compilerDesc.type)
        {
            const Int versionValue = compilerDesc.getVersionValue();
            switch (matchType)
            {
                case MatchType::MinGreaterEqual:
                {
                    auto diff = descVersionValue - versionValue;
                    if (diff >= 0 && diff < minVersionDiff)
                    {
                        bestIndex = i;
                        minVersionDiff = diff;
                    }
                    break;
                }
                case MatchType::MinAbsolute:
                {
                    auto diff = descVersionValue - versionValue;
                    diff = (diff >= 0) ? diff : -diff;
                    if (diff < minVersionDiff)
                    {
                        bestIndex = i;
                        minVersionDiff = diff;
                    }
                    break;
                }
                case MatchType::Newest:
                {
                    if (versionValue > maxVersionValue)
                    {
                        maxVersionValue = versionValue;
                        bestIndex = i;
                    }
                    break;
                }
            }
        }
    }

    return (bestIndex >= 0) ? compilers[bestIndex] : nullptr;
}

/* static */CPPCompiler* CPPCompilerUtil::findClosestCompiler(const List<CPPCompiler*>& compilers, const CPPCompiler::Desc& desc)
{
    CPPCompiler* compiler;

    compiler = findCompiler(compilers, MatchType::MinGreaterEqual, desc);
    if (compiler)
    {
        return compiler;
    }
    compiler = findCompiler(compilers, MatchType::MinAbsolute, desc);
    if (compiler)
    {
        return compiler;
    }

    // If we are gcc, we can try clang and vice versa
    if (desc.type == CPPCompiler::Type::GCC || desc.type == CPPCompiler::Type::Clang)
    {
        CPPCompiler::Desc compatible = desc;
        compatible.type = (compatible.type == CPPCompiler::Type::Clang) ? CPPCompiler::Type::GCC : CPPCompiler::Type::Clang;

        compiler = findCompiler(compilers, MatchType::MinGreaterEqual, compatible);
        if (compiler)
        {
            return compiler;
        }
        compiler = findCompiler(compilers, MatchType::MinAbsolute, compatible);
        if (compiler)
        {
            return compiler;
        }
    }

    return nullptr;
}

// Have to do this conditionally because unreferenced static functions are a warning on VC, and warnings are errors.
#if !SLANG_WINDOWS_FAMILY
static void _addGCCFamilyCompiler(const String& exeName, CPPCompilerSet* compilerSet)
{
    CPPCompiler::Desc desc;
    if (SLANG_SUCCEEDED(CPPCompilerUtil::calcGCCFamilyVersion(exeName, desc)))
    {
        RefPtr<CPPCompiler> compiler(new GenericCPPCompiler(desc, exeName, &CPPCompilerUtil::calcGCCFamilyArgs));
        compilerSet->addCompiler(compiler);
    }
}
#endif

/* static */CPPCompiler* CPPCompilerUtil::findClosestCompiler(const CPPCompilerSet* set, const CPPCompiler::Desc& desc)
{
    CPPCompiler* compiler = set->getCompiler(desc);
    if (compiler)
    {
        return compiler;
    }
    List<CPPCompiler*> compilers;
    set->getCompilers(compilers);
    return findClosestCompiler(compilers, desc);
}

/* static */SlangResult CPPCompilerUtil::initializeSet(CPPCompilerSet* set)
{
#if SLANG_WINDOWS_FAMILY
    WinVisualStudioUtil::find(set);
#else
    _addGCCFamilyCompiler("clang", set);
    _addGCCFamilyCompiler("g++", set);
#endif

    // Set the default to the compiler closest to how this source was compiled
    set->setDefaultCompiler(findClosestCompiler(set, getCompiledWithDesc()));
    return SLANG_OK;
}

/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! CPPCompilerFactory !!!!!!!!!!!!!!!!!!!!!!*/


void CPPCompilerSet::getCompilerDescs(List<CPPCompiler::Desc>& outCompilerDescs) const
{
    outCompilerDescs.clear();
    for (CPPCompiler* compiler : m_compilers)
    {
        outCompilerDescs.add(compiler->getDesc());
    }
}

Index CPPCompilerSet::_findIndex(const CPPCompiler::Desc& desc) const
{
    const Index count = m_compilers.getCount();
    for (Index i = 0; i < count; ++i)
    { 
        if (m_compilers[i]->getDesc() == desc)
        {
            return i;
        }
    }
    return -1;
}

CPPCompiler* CPPCompilerSet::getCompiler(const CPPCompiler::Desc& compilerDesc) const
{
    const Index index = _findIndex(compilerDesc);
    return index >= 0 ? m_compilers[index] : nullptr;
}

void CPPCompilerSet::getCompilers(List<CPPCompiler*>& outCompilers) const
{
    outCompilers.clear();
    outCompilers.addRange((CPPCompiler*const*)m_compilers.begin(), m_compilers.getCount());
}

void CPPCompilerSet::addCompiler(CPPCompiler* compiler)
{
    const Index index = _findIndex(compiler->getDesc());
    if (index >= 0)
    {
        m_compilers[index] = compiler;
    }
    else
    {
        m_compilers.add(compiler);
    }
}

}