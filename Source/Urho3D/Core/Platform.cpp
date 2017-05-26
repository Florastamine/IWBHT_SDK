//
// Copyright (c) 2008-2017 the Urho3D project.
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
//

#include "../Precompiled.h"

#include "../Core/Platform.h"
#include "../Core/StringUtils.h"
#include "../IO/FileSystem.h"

#include <cstdio>
#include <fcntl.h>

#ifdef __APPLE__
    #include "TargetConditionals.h"
#endif

#if defined(_WIN32)
    #include <windows.h>
    #include <io.h>
    #include <shlobj.h>
    #if defined(__MINGW32__)
        #include <lmcons.h>
        #include <ntdef.h> 
    #endif
    #include <bitset>
    #include <vector>
#elif defined(__linux__) && !defined(__ANDROID__) 
    #include <pwd.h> 
    #include <sys/sysinfo.h>
    #include <sys/utsname.h>
    #include <fstream> 
    #include <string> 
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <SystemConfiguration/SystemConfiguration.h> // For the detection functions inside GetLoginName(). 
#endif

#ifndef _WIN32
    #include <unistd.h>
#endif 

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    #include <thread> 
#endif 

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    #include <emscripten/threading.h>
#endif

#if defined(__i386__)
// From http://stereopsis.com/FPU.html
#define FPU_CW_PREC_MASK        0x0300
#define FPU_CW_PREC_SINGLE      0x0000
#define FPU_CW_PREC_DOUBLE      0x0200
#define FPU_CW_PREC_EXTENDED    0x0300
#define FPU_CW_ROUND_MASK       0x0c00
#define FPU_CW_ROUND_NEAR       0x0000
#define FPU_CW_ROUND_DOWN       0x0400
#define FPU_CW_ROUND_UP         0x0800
#define FPU_CW_ROUND_CHOP       0x0c00

inline unsigned GetFPUState()
{
    unsigned control = 0;
    __asm__ __volatile__ ("fnstcw %0" : "=m" (control));
    return control;
}

inline void SetFPUState(unsigned control)
{
    __asm__ __volatile__ ("fldcw %0" : : "m" (control));
}
#endif

#if defined(_WIN32)
    #include <intrin.h> 

    static void               __GetCPUID__(int (&out)[4], int x) { __cpuidex(out, x, 0); }
    static unsigned long long __GetECRInfo__(unsigned x)             { return _xgetbv(x); } // https://software.intel.com/en-us/node/523373 
#elif defined(__linux__) && !defined(__ANDROID__) 
    #include <cpuid.h> 

    static void __GetCPUID__(int (&out)[4], int x)
    {
        __cpuid_count(x, 0, out[0], out[1], out[2], out[3]); 
    }

    static unsigned long long __GetECRInfo__(unsigned x)
    {
        unsigned eax, edx; 
        __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(x));
        return ((unsigned long long) edx << 32) | eax;
    }
#endif

#if defined(_WIN32) && defined(_MSC_VER)
    #define __PLATFORM__XCR_XFEATURE_ENABLED_MASK _XCR_XFEATURE_ENABLED_MASK 
#else 
    #define __PLATFORM__XCR_XFEATURE_ENABLED_MASK 0 
#endif 

static bool __GetAVXSupport__()
{
	bool has_avx = false;

	int c[4];
	__GetCPUID__(c, 1);

	bool osusesxsave_restore = (c[2] & (1 << 27)) != 0;
	bool cpusupportsavx = (c[2] & (1 << 28)) != 0;

	if (osusesxsave_restore && cpusupportsavx) 
    {
		unsigned long long xcrFeatureMask = __GetECRInfo__(__PLATFORM__XCR_XFEATURE_ENABLED_MASK);
		has_avx = (xcrFeatureMask & 0x6) == 0x6;
	}

	return has_avx;
}

static bool __GetAVX512Support__()
{
    if(!__GetAVXSupport__()) 
        return false;

	unsigned long long xcrFeatureMask = __GetECRInfo__(__PLATFORM__XCR_XFEATURE_ENABLED_MASK);
	return (xcrFeatureMask & 0xe6) == 0xe6;
}

#ifndef MINI_URHO
#include <SDL/SDL.h>
#endif

namespace Urho3D
{

#ifdef _WIN32
static bool consoleOpened = false;
#endif

static String currentLine;
static Vector<String> arguments;

struct CpuCoreCount
{
    unsigned numPhysicalCores_;
    unsigned numLogicalCores_;
};

#if defined(__linux__)
// This function is used by all the target triplets with Linux as the OS, such as Android, RPI, desktop Linux, etc
static void GetCPUData(struct CpuCoreCount* data)
{
    // Sanity check
    assert(data);
    // At least return 1 core
    data->numPhysicalCores_ = data->numLogicalCores_ = 1;

    FILE* fp;
    int res;
    unsigned i, j;

    fp = fopen("/sys/devices/system/cpu/present", "r");
    if (fp)
    {
        res = fscanf(fp, "%d-%d", &i, &j);
        fclose(fp);

        if (res == 2 && i == 0)
        {
            data->numPhysicalCores_ = data->numLogicalCores_ = j + 1;

            fp = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
            if (fp)
            {
                res = fscanf(fp, "%d,%d,%d,%d", &i, &j, &i, &j);
                fclose(fp);

                // Having sibling thread(s) indicates the CPU is using HT/SMT technology
                if (res > 1)
                    data->numPhysicalCores_ /= res;
            }
        }
    }
}

#elif defined(_WIN32) && !defined(__EMSCRIPTEN__)

static std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> __CPUInfoBuffer__()
{
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer;

	DWORD b = 0;
	GetLogicalProcessorInformation(nullptr, &b);

	buffer.resize(b / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
	GetLogicalProcessorInformation(buffer.data(), &b);

	return buffer;    
}

static CpuCoreCount __GetCPUInfo__()
{
    CpuCoreCount r{}; 
    auto b = __CPUInfoBuffer__();

    for(auto &&ca : b)
    {
        switch(ca.Relationship)
        {
            case RelationProcessorCore: 
            {
                ++(r.numPhysicalCores_); 
                r.numLogicalCores_ += static_cast<unsigned>(std::bitset<sizeof(ULONG_PTR) * 8>(ca.ProcessorMask).count());
                break; 
            }

            default: ;
        }
    }

    return r; 
}

static void GetCPUData(struct CpuCoreCount* data)
{
    *data = __GetCPUInfo__();
}
#endif

void InitFPU()
{
    // Make sure FPU is in round-to-nearest, single precision mode
    // This ensures Direct3D and OpenGL behave similarly, and all threads behave similarly
#if defined(__i386__)
    unsigned control = GetFPUState();
    control &= ~(FPU_CW_PREC_MASK | FPU_CW_ROUND_MASK);
    control |= (FPU_CW_PREC_SINGLE | FPU_CW_ROUND_NEAR);
    SetFPUState(control);
#endif
}

void ErrorDialog(const String& title, const String& message)
{
#ifndef MINI_URHO
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.CString(), message.CString(), 0);
#endif
}

void ErrorExit(const String& message, int exitCode)
{
    if (!message.Empty())
        PrintLine(message, true);

    exit(exitCode);
}

void OpenConsoleWindow()
{
#if defined(_WIN32)
    if (consoleOpened)
        return;

    AllocConsole();

    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);

    consoleOpened = true;
#endif
}

void PrintUnicode(const String& str, bool error)
{
#if defined(_WIN32)
    // If the output stream has been redirected, use fprintf instead of WriteConsoleW,
    // though it means that proper Unicode output will not work
    FILE* out = error ? stderr : stdout;
    if (!_isatty(_fileno(out)))
        fprintf(out, "%s", str.CString());
    else
    {
        HANDLE stream = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (stream == INVALID_HANDLE_VALUE)
            return;
        WString strW(str);
        DWORD charsWritten;
        WriteConsoleW(stream, strW.CString(), strW.Length(), &charsWritten, 0);
    }
#elif defined(__linux__)
    fprintf(error ? stderr : stdout, "%s", str.CString());
#endif
}

void PrintUnicodeLine(const String& str, bool error)
{
    PrintUnicode(str + "\n", error);
}

void PrintLine(const String& str, bool error)
{
    fprintf(error ? stderr : stdout, "%s\n", str.CString());
}

const Vector<String>& ParseArguments(const String& cmdLine, bool skipFirstArgument)
{
    arguments.Clear();

    unsigned cmdStart = 0, cmdEnd = 0;
    bool inCmd = false;
    bool inQuote = false;

    for (unsigned i = 0; i < cmdLine.Length(); ++i)
    {
        if (cmdLine[i] == '\"')
            inQuote = !inQuote;
        if (cmdLine[i] == ' ' && !inQuote)
        {
            if (inCmd)
            {
                inCmd = false;
                cmdEnd = i;
                // Do not store the first argument (executable name)
                if (!skipFirstArgument)
                    arguments.Push(cmdLine.Substring(cmdStart, cmdEnd - cmdStart));
                skipFirstArgument = false;
            }
        }
        else
        {
            if (!inCmd)
            {
                inCmd = true;
                cmdStart = i;
            }
        }
    }
    if (inCmd)
    {
        cmdEnd = cmdLine.Length();
        if (!skipFirstArgument)
            arguments.Push(cmdLine.Substring(cmdStart, cmdEnd - cmdStart));
    }

    // Strip double quotes from the arguments
    for (unsigned i = 0; i < arguments.Size(); ++i)
        arguments[i].Replace("\"", "");

    return arguments;
}

const Vector<String>& ParseArguments(const char* cmdLine)
{
    return ParseArguments(String(cmdLine));
}

const Vector<String>& ParseArguments(const WString& cmdLine)
{
    return ParseArguments(String(cmdLine));
}

const Vector<String>& ParseArguments(const wchar_t* cmdLine)
{
    return ParseArguments(String(cmdLine));
}

const Vector<String>& ParseArguments(int argc, char** argv)
{
    String cmdLine;

    for (int i = 0; i < argc; ++i)
        cmdLine.AppendWithFormat("\"%s\" ", (const char*)argv[i]);

    return ParseArguments(cmdLine);
}

const Vector<String>& GetArguments()
{
    return arguments;
}

String GetConsoleInput()
{
    String ret;
#if defined(_WIN32)
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE)
        return ret;

    // Use char-based input
    SetConsoleMode(input, ENABLE_PROCESSED_INPUT);

    INPUT_RECORD record;
    DWORD events = 0;
    DWORD readEvents = 0;

    if (!GetNumberOfConsoleInputEvents(input, &events))
        return ret;

    while (events--)
    {
        ReadConsoleInputW(input, &record, 1, &readEvents);
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
        {
            unsigned c = record.Event.KeyEvent.uChar.UnicodeChar;
            if (c)
            {
                if (c == '\b')
                {
                    PrintUnicode("\b \b");
                    int length = currentLine.LengthUTF8();
                    if (length)
                        currentLine = currentLine.SubstringUTF8(0, length - 1);
                }
                else if (c == '\r')
                {
                    PrintUnicode("\n");
                    ret = currentLine;
                    currentLine.Clear();
                    return ret;
                }
                else
                {
                    // We have disabled echo, so echo manually
                    wchar_t out = c;
                    DWORD charsWritten;
                    WriteConsoleW(output, &out, 1, &charsWritten, 0);
                    currentLine.AppendUTF8(c);
                }
            }
        }
    }
#elif !defined(__ANDROID__) && !defined(IOS)
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    for (;;)
    {
        int ch = fgetc(stdin);
        if (ch >= 0 && ch != '\n')
            ret += (char)ch;
        else
            break;
    }
#endif

    return ret;
}

String GetPlatform()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Mac OS X";
#elif defined(__linux__)
    return "Linux";
#else
    return "(Unknown OS)"; 
#endif
}

unsigned GetNumCPUCores()
{
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    struct CpuCoreCount data;
    GetCPUData(&data);
    return data.numPhysicalCores_;
#elif defined(__EMSCRIPTEN__)
    #ifdef __EMSCRIPTEN_PTHREADS__
        return emscripten_num_logical_cores();
    #else
        return 1; // Targeting a single-threaded Emscripten build.
    #endif 
#endif 
    return 1;
}

unsigned GetNumCPUThreads()
{
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    return std::thread::hardware_concurrency();
#elif defined(__EMSCRIPTEN__)
    #ifdef __EMSCRIPTEN_PTHREADS__
        return emscripten_num_logical_cores();
    #else
        return 1; // Targeting a single-threaded Emscripten build.
    #endif
#endif 
    return 1; 
}

unsigned long long GetTotalMemory()
{
#if defined(__linux__) && !defined(__ANDROID__)
    struct sysinfo s;
    if (sysinfo(&s) != -1)
        return s.totalram; 
#elif defined(_WIN32)
    MEMORYSTATUSEX state;
    state.dwLength = sizeof(state); 
    if (GlobalMemoryStatusEx(&state)) 
        return state.ullTotalPhys; 
#elif defined(__APPLE__)
    unsigned long long memSize;
    size_t len = sizeof(memSize);
    int mib[2]; 
    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;
    sysctl(mib, 2, &memSize, &len, NULL, 0);
    return memSize;
#endif
    return 0ull;
}

String GetLoginName() 
{
#if defined(__linux__) && !defined(__ANDROID__)
    struct passwd *p = getpwuid(getuid());
    if (p) 
        return p->pw_name;
#elif defined(_WIN32)
    char name[UNLEN + 1];
    DWORD len = UNLEN + 1;
    if (GetUserName(name, &len))
        return name;
#elif defined(__APPLE__) && !defined(IOS)
    SCDynamicStoreRef s = SCDynamicStoreCreate(NULL, CFSTR("GetConsoleUser"), NULL, NULL);
    if (s != NULL)
    {
        uid_t u; 
        CFStringRef n = SCDynamicStoreCopyConsoleUser(s, &u, NULL);
        CFRelease(s); 
        if (n != NULL)
        {
            char name[256]; 
            Boolean b = CFStringGetCString(n, name, 256, kCFStringEncodingUTF8);
            CFRelease(n); 

            if (b == true)
                return name; 
        }
    }
#endif
    return "(?)"; 
}

String GetHostName() 
{
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__)
    char buffer[256]; 
    if (gethostname(buffer, 256) == 0) 
        return buffer; 
#elif defined(_WIN32)
    char buffer[MAX_COMPUTERNAME_LENGTH + 1]; 
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1; 
    if (GetComputerName(buffer, &len))
        return buffer;
#endif
    return "(?)"; 
}

#if defined(_WIN32)
typedef NTSTATUS (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

static void GetOS(RTL_OSVERSIONINFOW *r)
{
    HMODULE m = GetModuleHandle("ntdll.dll");
    if (m)
    {
        RtlGetVersionPtr fPtr = (RtlGetVersionPtr) GetProcAddress(m, "RtlGetVersion");
        if (r && fPtr && fPtr(r) == 0)
            r->dwOSVersionInfoSize = sizeof *r; 
    }
}
#endif 

String GetOSVersion() 
{
#if defined(__linux__) && !defined(__ANDROID__)
    struct utsname u;
    if (uname(&u) == 0)
        return String(u.sysname) + " " + u.release; 
#elif defined(_WIN32)
    RTL_OSVERSIONINFOW r;
    GetOS(&r); 
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms724832(v=vs.85).aspx
    if (r.dwMajorVersion == 5 && r.dwMinorVersion == 0) 
        return "Windows 2000"; 
    else if (r.dwMajorVersion == 5 && r.dwMinorVersion == 1) 
        return "Windows XP"; 
    else if (r.dwMajorVersion == 5 && r.dwMinorVersion == 2) 
        return "Windows XP 64-Bit Edition/Windows Server 2003/Windows Server 2003 R2"; 
    else if (r.dwMajorVersion == 6 && r.dwMinorVersion == 0) 
        return "Windows Vista/Windows Server 2008"; 
    else if (r.dwMajorVersion == 6 && r.dwMinorVersion == 1) 
        return "Windows 7/Windows Server 2008 R2"; 
    else if (r.dwMajorVersion == 6 && r.dwMinorVersion == 2) 
        return "Windows 8/Windows Server 2012";
    else if (r.dwMajorVersion == 6 && r.dwMinorVersion == 3) 
        return "Windows 8.1/Windows Server 2012 R2"; 
    else if (r.dwMajorVersion == 10 && r.dwMinorVersion == 0) 
        return "Windows 10/Windows Server 2016"; 
    else 
        return "Windows (unknown version)";
#elif defined(__APPLE__)
    char kernel_r[256]; 
    size_t size = sizeof(kernel_r); 

    if (sysctlbyname("kern.osrelease", &kernel_r, &size, NULL, 0) != -1)
    {
        Vector<String> kernel_version = String(kernel_r).Split('.'); 
        String version = "macOS/Mac OS X "; 
        int major = ToInt(kernel_version[0]);
        int minor = ToInt(kernel_version[1]);

        // https://en.wikipedia.org/wiki/Darwin_(operating_system)
        if (major == 16) // macOS Sierra 
        {
            version += "Sierra "; 
            switch(minor)
            {
                case 0: version += "10.12.0 "; break; 
                case 1: version += "10.12.1 "; break; 
                case 3: version += "10.12.2 "; break; 
            }
        }
        else if (major == 15) // OS X El Capitan
        {
            version += "El Capitan ";
            switch(minor)
            {
                case 0: version += "10.11.0 "; break; 
                case 6: version += "10.11.6 "; break; 
            }
        }
        else if (major == 14) // OS X Yosemite 
        {
            version += "Yosemite "; 
            switch(minor) 
            {
                case 0: version += "10.10.0 "; break; 
                case 5: version += "10.10.5 "; break; 
            }
        }
        else if (major == 13) // OS X Mavericks
        {
            version += "Mavericks ";
            switch(minor)
            {
                case 0: version += "10.9.0 "; break; 
                case 4: version += "10.9.5 "; break; 
            }
        }
        else if (major == 12) // OS X Mountain Lion
        {
            version += "Mountain Lion "; 
            switch(minor) 
            {
                case 0: version += "10.8.0 "; break; 
                case 6: version += "10.8.5 "; break; 
            }
        }
        else if (major == 11) // Mac OS X Lion
        {
            version += "Lion ";
            switch(minor)
            {
                case 0: version += "10.7.0 "; break; 
                case 4: version += "10.7.5 "; break; 
            }
        }
        else 
        {
            version += "Unknown ";
        }

        return version + " (Darwin kernel " + kernel_version[0] + "." + kernel_version[1] + "." + kernel_version[2] + ")"; 
    }
#endif
    return "(Unknown OS version)"; 
}

String GetHomePath()
{
#if (defined(__linux__) /* || defined(__APPLE__) */) && !defined(__ANDROID__) 
    const char *c; 
    if((c = getenv("HOME")) == NULL)
    {
        struct passwd *pwd = getpwuid(getuid());
        c = (pwd != NULL) ? pwd->pw_dir : ""; 
    }
    return String(c) + "/"; 
#elif defined(_WIN32)
    char path[MAX_PATH + 1];
    if (SHGetSpecialFolderPathA(HWND_DESKTOP, path, CSIDL_DESKTOP, FALSE))
        return String(path) + "\\"; 
#endif
    return String::EMPTY; 
}

String GetTemporaryPath()
{
#if defined(__linux__) && !defined(__ANDROID__) 
    const char *c;
    if((c = getenv("TMPDIR")) == NULL)
        return String(P_tmpdir) + "/"; 
    else 
        return c;
#elif defined(_WIN32)
    char buffer[MAX_PATH]; // Actually, GetTempPath() takes a TCHAR[] but I don't care about catering to users with Unicode 
                           // characters (very unlikely, and that would only complicate the implementation) 
                           // in their user name so I went straight to char[]. 
    DWORD r = GetTempPath(MAX_PATH, buffer);
    if(!(r > MAX_PATH || (r == 0)))
        return buffer; // GetTempPath() have already filled in the ending '\\', so we just return the buffer. 
#endif
    return String::EMPTY; 
}

bool GetCPUBigEndian()
{
    int n = 1;
    return (*(char *)&n == 1) ? false : true; 
}

bool GetCPULittleEndian()
{
    return !GetCPUBigEndian(); 
}

unsigned long long GetCPUClock()
{
#if defined(__linux__) && !defined(__ANDROID__)
    std::ifstream cpuinfo("/proc/cpuinfo"); 
    if(cpuinfo.is_open())
    {
        for(std::string line; std::getline(cpuinfo, line);)
        {
            if(0 == line.find("cpu MHz"))
                return static_cast<unsigned long long>(std::strtod(line.c_str() + line.find_first_of(':') + 1, nullptr)); 
        }
    }
#elif defined(_WIN32)
    LARGE_INTEGER f; 
    QueryPerformanceFrequency(&f); 
    return static_cast<unsigned long long>(f.QuadPart / 1000.0); 
#endif
    return 0ull;
}

String GetCPUArchitecture()
{
#if defined(__linux__) && !defined(__ANDROID__) 
    struct utsname u;
    if (uname(&u) == 0)
    {
        String s(u.machine);
        if(s.Contains("x86_64", false))
            return "x86_64"; 
        else if(s.Contains("IA64", false))
            return "IA64"; 
        else if(s.Contains("i686", false))
            return "x86"; 
        else if(s.Contains("ARM", false))
            return "ARM"; 
    }
#elif defined(_WIN32)
    SYSTEM_INFO s;
    GetNativeSystemInfo(&s); // Because GetSystemInfo() would report incorrect results (x86 instead of x86_64) when running the application under WOW64 
                             // environment (e.g. running 32-bit software under 64-bit OS). 

    switch(s.wProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_AMD64 : return "x86_64";
        case PROCESSOR_ARCHITECTURE_IA64  : return "IA64"; 
        case PROCESSOR_ARCHITECTURE_INTEL : return "x86";
        case PROCESSOR_ARCHITECTURE_ARM   : return "ARM"; 
        default: ;
    }
#endif
    return "(Unknown architecture)"; 
}

String GetCPUVendorID()
{
    int c[4];
    __GetCPUID__(c, 0); 

    String s((const char *) &(c[1]), 4);
    s += String((const char *) &(c[3]), 4);
    s += String((const char *) &(c[2]), 4); 

    return s;
}

String GetCPUExtensions()
{
    String s(String::EMPTY); 

	int c[4];
	__GetCPUID__(c, 0);
    int id = c[0]; 

    __GetCPUID__(c, 0x80000000); 
    unsigned ext_id = c[0]; 

    #define ADD_IF(iindex, bindex, str) \
        if ((c[(iindex)] & ((int)1 << (bindex)) ) != 0) { if(!s.Contains(str)) s = s + str + ";"; }
    
	if (id >= 0x00000001)
    {
		__GetCPUID__(c, 0x00000001);
		ADD_IF(3, 22, "Extended MMX");
		ADD_IF(3, 23, "MMX");
		ADD_IF(3, 25, "SSE");
		ADD_IF(3, 26, "SSE2");
		ADD_IF(2, 0, "SSE3");
		ADD_IF(2, 9, "SSSE3");
		ADD_IF(2, 19, "SSE4.1");
		ADD_IF(2, 20, "SSE4.2");
		ADD_IF(2, 25, "AES");
		ADD_IF(2, 28, "FMA3");
		ADD_IF(2, 12, "RDRAND");
	}
	if (id >= 0x00000007)
    {
		__GetCPUID__(c, 0x00000007);

        if(__GetAVXSupport__())
            ADD_IF(1, 5, "AVX2");

		ADD_IF(1, 4, "HLE");
		ADD_IF(1, 3, "BMI");
		ADD_IF(1, 8, "BMI2");
		ADD_IF(1, 19, "ADX");
		ADD_IF(1, 14, "MPX");
		ADD_IF(1, 29, "SHA");

		if (__GetAVX512Support__())
        {
            s = s + "AVX-512;"; 
			ADD_IF(1, 16, "AVX-512 F");
			ADD_IF(1, 28, "AVX-512 CDI");
			ADD_IF(1, 26, "AVX-512 PFI");
			ADD_IF(1, 27, "AVX-512 ERI");
			ADD_IF(1, 31, "AVX-512 VL");
			ADD_IF(1, 30, "AVX-512 BW");
			ADD_IF(1, 17, "AVX-512 DQ");
			ADD_IF(1, 21, "AVX-512 IFMA");
			ADD_IF(2, 1, "AVX-512 VBMI");
		}

		ADD_IF(2, 0, "PREFETCHWT1");
	}
	if (ext_id >= 0x80000001) 
    {
		__GetCPUID__(c, 0x80000001);

		ADD_IF(3, 29, "EM64T");
		ADD_IF(3, 0, "x87");
		ADD_IF(3, 30, "3DNow!");
		ADD_IF(3, 31, "Extended 3DNow!");
		ADD_IF(2, 5, "BMI");
		ADD_IF(2, 6, "BMI2");
		ADD_IF(2, 16, "ADX");
		ADD_IF(2, 11, "MPX");
	}
    #undef ADD_IF 

    s.Erase(s.Length() - 1);
    return s;
}

}