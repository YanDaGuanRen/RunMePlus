// ============================================================================
// RunMeCpp —— RunMe 启动器的 C++/Win32 单文件实现
// ----------------------------------------------------------------------------
// 对照 C# 版（ShowForm.cs / Program.cs）移植，遵循同一套行为语义：
//   - 单 exe 多身份：exe 文件名 → [Config] 同名键 → 启动目标
//   - 执行标记：runadmin / show / cmd / ps / powershell（顺序任意）
//   - 路径前缀：pf\ / pf86\ / AppData / ..\ / 绝对路径
// 构建目标：/MT 静态链接、无运行时依赖的单文件 exe
// ----------------------------------------------------------------------------
// 已全部实现：配置读取/首启生成、路径解析、占位符与 {0} 填充、标记解析、
//              cmd/ps 执行、run.txt、runme/list 列表窗口（Win32 ListBox）、
//              runmeth/runmefth、help、多身份带参；对照 docs/run-tests.ps1 黑盒测试
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdlib>

// ============================================================================
// 基础工具
// ============================================================================

static bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    return _wcsnicmp(s.c_str(), prefix, n) == 0;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == L' ' || s[begin] == L'\t' || s[begin] == L'\r'))
        ++begin;
    while (end > begin && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r'))
        --end;
    return s.substr(begin, end - begin);
}

// 解析十进制整数；非数字时返回 fallback（配置项宽容解析）
static int ParseIntOrDefault(const std::wstring& s, int fallback)
{
    std::wstring t = Trim(s);
    if (t.empty()) return fallback;

    wchar_t* end = nullptr;
    long v = wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return fallback;
    return (int)v;
}

static std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;
    for (auto& c : r) c = towlower(c);
    return r;
}

// 与 C# string.Split 类似：按单字符切分；removeEmpty=true 时丢弃空段
static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep, bool removeEmpty)
{
    std::vector<std::wstring> result;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == sep)
        {
            std::wstring piece = s.substr(start, i - start);
            if (!removeEmpty || !piece.empty())
                result.push_back(piece);
            start = i + 1;
        }
    }
    return result;
}

// 取扩展名（含点）；对照 .NET Path.GetExtension。目录分隔符之后的最后一个点才算。
static std::wstring GetExtension(const std::wstring& path)
{
    size_t lastDot = path.find_last_of(L'.');
    if (lastDot == std::wstring::npos) return L"";
    size_t lastSep = path.find_last_of(L"\\/");
    if (lastSep != std::wstring::npos && lastDot < lastSep) return L"";
    return path.substr(lastDot);
}

// 去掉扩展名；对照 .NET Path.GetFileNameWithoutExtension 的"作用于文件名"语义
static std::wstring RemoveExtension(const std::wstring& name)
{
    size_t lastDot = name.find_last_of(L'.');
    size_t lastSep = name.find_last_of(L"\\/");
    if (lastDot == std::wstring::npos) return name;
    if (lastSep != std::wstring::npos && lastDot < lastSep) return name;
    return name.substr(0, lastDot);
}

// 取文件名部分（含扩展名）
static std::wstring GetFileName(const std::wstring& path)
{
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

// 取目录部分（含尾部反斜杠），对照 AppDomain.BaseDirectory 的用法
static std::wstring GetDirectory(const std::wstring& path)
{
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"" : path.substr(0, sep + 1);
}

// 对照 .NET Path.Combine（简单场景）：保证单分隔符连接
static std::wstring CombinePath(const std::wstring& dir, const std::wstring& rest)
{
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    std::wstring r = rest;
    while (!r.empty() && (r.front() == L'\\' || r.front() == L'/')) r.erase(r.begin());
    if (d.empty()) return r;
    if (r.empty()) return d;
    return d + L"\\" + r;
}

// 上一级目录（对照 .NET Directory.GetParent(dir).FullName：先忽略结尾多余分隔符）
static std::wstring GetParentDir(const std::wstring& dir)
{
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    size_t sep = d.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return dir;
    return d.substr(0, sep);
}

static bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 简单消息框（对照 C# MessageBox.Show）
static void ShowMessageBox(const std::wstring& text, const std::wstring& caption = L"")
{
    MessageBoxW(nullptr, text.c_str(), caption.c_str(), MB_OK);
}

// 裸命令判定：无路径分隔符/盘符且无扩展名（对照 C# 版的判定）
static bool IsBareCommand(const std::wstring& s)
{
    return s.find_first_of(L"\\/:") == std::wstring::npos && GetExtension(s).empty();
}

// ============================================================================
// 配置（INI）
// ============================================================================

struct CaseInsensitiveLess
{
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

using SectionMap = std::map<std::wstring, std::wstring, CaseInsensitiveLess>;
using ConfigMap = std::map<std::wstring, SectionMap, CaseInsensitiveLess>;

// 按 UTF-8 读取整个文件并转宽字符（跳过 BOM）
static bool ReadFileUtf8(const std::wstring& path, std::wstring& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(h);
        return false;
    }

    std::vector<char> buf(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = buf.empty() ? TRUE : ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;

    size_t offset = 0;
    if (read >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        offset = 3; // 跳过 UTF-8 BOM

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data() + offset, static_cast<int>(read - offset), nullptr, 0);
    if (wlen <= 0 && read > offset) return false;
    out.assign(wlen, L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, 0, buf.data() + offset, static_cast<int>(read - offset), out.data(), wlen);
    return true;
}

// 解析 INI：空行与 #/; 开头行跳过；节、键不区分大小写；值 = 第一个 '=' 之后全部
static void ParseConfigText(const std::wstring& text, ConfigMap& config)
{
    std::wstring currentSection;
    SectionMap* current = nullptr;

    size_t start = 0;
    while (start <= text.size())
    {
        size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, (nl == std::wstring::npos ? text.size() : nl) - start);
        start = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;

        line = Trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        if (line.front() == L'[' && line.back() == L']')
        {
            currentSection = line.substr(1, line.size() - 2);
            current = &config[currentSection];
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0 && !currentSection.empty())
        {
            if (!current) current = &config[currentSection];
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring value = Trim(line.substr(eq + 1));
            (*current)[key] = value;
        }
    }
}

// ============================================================================
// 全局状态
// ============================================================================

static std::wstring g_exePath;    // exe 完整路径（对照 Application.ExecutablePath）
static std::wstring g_exeDir;     // exe 所在目录（带尾反斜杠），对照 AppDomain.BaseDirectory
static std::wstring g_exeName;    // exe 文件名（无扩展名），对照 RunExeName
static std::wstring g_cfgPath;    // YanBinCfg.ini 完整路径
static std::wstring g_parentDir;  // [Settings] RunParentDirectory
static ConfigMap g_config;

// 命令行参数：供 {0}{1}… 填充或追加到命令尾部（对照 _extraArgs）
static std::vector<std::wstring> g_extraArgs;
static bool g_hasExtraArgs = false;

static bool ReadValue(const std::wstring& section, const std::wstring& key, std::wstring& value)
{
    auto s = g_config.find(section);
    if (s == g_config.end()) return false;
    auto k = s->second.find(key);
    if (k == s->second.end()) return false;
    value = k->second;
    return true;
}

// ============================================================================
// 通用小工具（占位符 / 参数填充 / 字符串处理）
// ============================================================================

// 数字转字符串（width>0 时左补零）
static std::wstring Num(long long value, int width)
{
    wchar_t buf[32];
    if (width > 0)
        swprintf(buf, 32, L"%0*lld", width, value);
    else
        swprintf(buf, 32, L"%lld", value);
    return buf;
}

// 去掉开头空白（对照 .NET String.TrimStart）
static std::wstring TrimStartWs(const std::wstring& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n'))
        ++i;
    return s.substr(i);
}

// 去掉首尾空白（对照 .NET String.Trim）
static std::wstring TrimWs(const std::wstring& s)
{
    std::wstring r = TrimStartWs(s);
    while (!r.empty() && (r.back() == L' ' || r.back() == L'\t' || r.back() == L'\r' || r.back() == L'\n'))
        r.pop_back();
    return r;
}

// 去掉开头指定字符（对照 .NET String.TrimStart(char)）
static std::wstring TrimStartChar(const std::wstring& s, wchar_t c)
{
    size_t i = 0;
    while (i < s.size() && s[i] == c) ++i;
    return s.substr(i);
}

// 拼接（对照 .NET String.Join）
static std::wstring Join(const std::vector<std::wstring>& parts, const wchar_t* sep)
{
    std::wstring r;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) r += sep;
        r += parts[i];
    }
    return r;
}

// 生成新 GUID（对照 Guid.NewGuid().ToString()：小写、无花括号）
static std::wstring NewGuidString()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return L"";
    wchar_t buf[64];
    swprintf(buf, 64, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned)guid.Data1, (unsigned)guid.Data2, (unsigned)guid.Data3,
             (unsigned)guid.Data4[0], (unsigned)guid.Data4[1],
             (unsigned)guid.Data4[2], (unsigned)guid.Data4[3],
             (unsigned)guid.Data4[4], (unsigned)guid.Data4[5],
             (unsigned)guid.Data4[6], (unsigned)guid.Data4[7]);
    return buf;
}

// 随机数生成器（对照 new Random().Next(min, max+1)）
static std::mt19937 g_rng{ std::random_device{}() };

static long long RandomBetween(long long minV, long long maxV)
{
    if (minV > maxV) std::swap(minV, maxV);
    std::uniform_int_distribution<long long> dist(minV, maxV);
    return dist(g_rng);
}

// ---- .NET 日期格式（DateTime.Now.ToString(format) 的常用子集） ----

static std::wstring FormatOffset()
{
    TIME_ZONE_INFORMATION tzi{};
    DWORD r = GetTimeZoneInformation(&tzi);
    long bias = tzi.Bias;
    if (r == TIME_ZONE_ID_DAYLIGHT) bias += tzi.DaylightBias;
    else if (r == TIME_ZONE_ID_STANDARD) bias += tzi.StandardBias;
    long offset = -bias; // 本地相对 UTC 的偏移（分钟）
    wchar_t sign = offset < 0 ? L'-' : L'+';
    if (offset < 0) offset = -offset;
    return std::wstring(1, sign) + Num(offset / 60, 2) + L":" + Num(offset % 60, 2);
}

static std::wstring FormatRfc1123(const SYSTEMTIME& st)
{
    static const wchar_t* days[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    static const wchar_t* months[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    (void)st;
    int dow = utc.wDayOfWeek % 7;
    int mon = (utc.wMonth >= 1 && utc.wMonth <= 12) ? utc.wMonth - 1 : 0;
    wchar_t buf[128];
    swprintf(buf, 128, L"%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[dow], utc.wDay, months[mon], utc.wYear, utc.wHour, utc.wMinute, utc.wSecond);
    return buf;
}

static std::wstring FormatStandardChar(wchar_t c, const SYSTEMTIME& st, bool& ok)
{
    wchar_t buf[128];
    ok = true;

    auto dateFmt = [&](DWORD flags) -> std::wstring {
        if (GetDateFormatW(LOCALE_USER_DEFAULT, flags, &st, nullptr, buf, 128) > 0) return buf;
        return L"";
    };
    auto timeFmt = [&](DWORD flags) -> std::wstring {
        if (GetTimeFormatW(LOCALE_USER_DEFAULT, flags, &st, nullptr, buf, 128) > 0) return buf;
        return L"";
    };
    auto iso = [&](const SYSTEMTIME& t) -> std::wstring {
        return Num(t.wYear, 4) + L"-" + Num(t.wMonth, 2) + L"-" + Num(t.wDay, 2) + L"T" +
               Num(t.wHour, 2) + L":" + Num(t.wMinute, 2) + L":" + Num(t.wSecond, 2);
    };

    switch (c)
    {
    case L'd': return dateFmt(DATE_SHORTDATE);
    case L'D': return dateFmt(DATE_LONGDATE);
    case L't': return timeFmt(TIME_NOMINUTESORSECONDS);
    case L'T': return timeFmt(TIME_NOSECONDS);
    case L'f': return dateFmt(DATE_LONGDATE) + L" " + timeFmt(TIME_NOMINUTESORSECONDS);
    case L'F': return dateFmt(DATE_LONGDATE) + L" " + timeFmt(TIME_NOSECONDS);
    case L'g': return dateFmt(DATE_SHORTDATE) + L" " + timeFmt(TIME_NOMINUTESORSECONDS);
    case L'G': return dateFmt(DATE_SHORTDATE) + L" " + timeFmt(TIME_NOSECONDS);
    case L'm':
    case L'M': return Num(st.wMonth, 0) + L"月" + Num(st.wDay, 0) + L"日";
    case L'y':
    case L'Y': return Num(st.wYear, 0) + L"年" + Num(st.wMonth, 0) + L"月";
    case L's': return iso(st);
    case L'u':
    {
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        return iso(utc) + L"Z";
    }
    case L'r':
    case L'R': return FormatRfc1123(st);
    case L'o':
    case L'O': return iso(st) + L"." + Num(st.wMilliseconds, 3) + L"0000" + FormatOffset();
    default:
        ok = false;
        return L"";
    }
}

// 自定义格式串：输出为空串表示格式非法（调用方保持占位符原样）
static std::wstring FormatDotNetTime(const std::wstring& format, const SYSTEMTIME& st)
{
    if (format.empty()) return L"";

    // 单字符 = .NET 标准格式串
    if (format.size() == 1)
    {
        bool ok = false;
        std::wstring r = FormatStandardChar(format[0], st, ok);
        return ok ? r : L"";
    }

    std::wstring out;
    for (size_t i = 0; i < format.size(); )
    {
        wchar_t c = format[i];
        size_t j = i;
        while (j < format.size() && format[j] == c) ++j;
        int count = (int)(j - i);

        switch (c)
        {
        case L'y':
            out += (count <= 2) ? Num(st.wYear % 100, count == 2 ? 2 : 0) : Num(st.wYear, 4);
            break;
        case L'M':
            out += Num(st.wMonth, count >= 2 ? 2 : 0);
            break;
        case L'd':
            out += Num(st.wDay, count >= 2 ? 2 : 0);
            break;
        case L'H':
            out += Num(st.wHour, count >= 2 ? 2 : 0);
            break;
        case L'h':
        {
            int h = st.wHour % 12;
            if (h == 0) h = 12;
            out += Num(h, count >= 2 ? 2 : 0);
            break;
        }
        case L'm':
            out += Num(st.wMinute, count >= 2 ? 2 : 0);
            break;
        case L's':
            out += Num(st.wSecond, count >= 2 ? 2 : 0);
            break;
        case L'f':
        {
            int ms = st.wMilliseconds;
            if (count <= 3)
            {
                for (int k = count; k < 3; ++k) ms /= 10;
                out += Num(ms, count);
            }
            else
            {
                out += Num(ms, 3);
                out += std::wstring((size_t)(count - 3), L'0');
            }
            break;
        }
        case L't':
            out += (st.wHour < 12) ? (count >= 2 ? L"AM" : L"A")
                                   : (count >= 2 ? L"PM" : L"P");
            break;
        default:
            out += std::wstring((size_t)count, c);
            break;
        }

        i = j;
    }
    return out;
}

// ---- 占位符 / {0} 参数填充 ----

// 对照正则 ^(-?\d+)-(-?\d+)$（{random.min-max}）
static bool ParseRandomRange(const std::wstring& spec, long long& minV, long long& maxV)
{
    size_t i = 0;
    auto readNum = [&](long long& out) -> bool {
        bool neg = false;
        if (i < spec.size() && spec[i] == L'-') { neg = true; ++i; }
        size_t start = i;
        long long value = 0;
        while (i < spec.size() && iswdigit(spec[i]))
        {
            value = value * 10 + (spec[i] - L'0');
            ++i;
        }
        if (i == start) return false;
        out = neg ? -value : value;
        return true;
    };

    if (!readNum(minV)) return false;
    if (i >= spec.size() || spec[i] != L'-') return false;
    ++i;
    if (!readNum(maxV)) return false;
    return i == spec.size();
}

// 占位符替换（对照 C# ProcessPlaceholders 正则 \{([^}]+\.[^}]+)\}）
static std::wstring ProcessPlaceholders(const std::wstring& input)
{
    if (input.empty()) return input;

    std::wstring out;
    size_t i = 0;
    while (i < input.size())
    {
        if (input[i] == L'{')
        {
            size_t close = input.find(L'}', i + 1);
            if (close != std::wstring::npos)
            {
                std::wstring content = input.substr(i + 1, close - i - 1);
                size_t dot = content.find(L'.');

                // 占位符必须含一个点（且点不在首尾），否则按普通字符处理
                if (dot != std::wstring::npos && dot > 0 && dot + 1 < content.size())
                {
                    bool matched = false;
                    std::wstring replaced;

                    if (StartsWithNoCase(content, L"time."))
                    {
                        SYSTEMTIME st{};
                        GetLocalTime(&st);
                        replaced = FormatDotNetTime(content.substr(5), st);
                        matched = !replaced.empty();   // 格式非法 → 保持原样
                    }
                    else if (StartsWithNoCase(content, L"env."))
                    {
                        if (!ReadValue(L"Settings", content.substr(4), replaced))
                            replaced.clear();          // 未定义 → 空串（对照 ?? ""）
                        matched = true;
                    }
                    else if (StartsWithNoCase(content, L"guid."))
                    {
                        replaced = NewGuidString();
                        matched = true;
                    }
                    else if (StartsWithNoCase(content, L"random."))
                    {
                        long long mn = 0, mx = 0;
                        if (ParseRandomRange(content.substr(7), mn, mx))
                        {
                            replaced = Num(RandomBetween(mn, mx), 0);
                            matched = true;
                        }
                    }

                    if (matched)
                    {
                        out += replaced;
                        i = close + 1;
                        continue;
                    }
                }

                // 未匹配：先按普通字符输出 '{'，后续字符继续扫描（内部仍可能有有效占位符）
                out += L'{';
                ++i;
                continue;
            }
        }

        out += input[i];
        ++i;
    }
    return out;
}

// {0}{1}… 占位符最大索引 + 1（对照 C# GetFormatParameterCount）
static int GetFormatParameterCount(const std::wstring& format)
{
    int maxIndex = -1;
    for (size_t i = 0; i < format.size(); ++i)
    {
        if (format[i] != L'{') continue;

        size_t j = i + 1;
        if (j >= format.size() || !iswdigit(format[j])) continue;

        long v = 0;
        while (j < format.size() && iswdigit(format[j]))
        {
            v = v * 10 + (format[j] - L'0');
            ++j;
        }
        if (j < format.size() && format[j] == L'}' && v > maxIndex)
            maxIndex = (int)v;
    }
    return maxIndex + 1;
}

// 把命令行参数填入 {0}{1}…（不足补空格；格式非法时原样返回，对照 C# FillFormatArgs）
static std::wstring FillFormatArgs(const std::wstring& format, int requiredParams)
{
    std::wstring out;
    for (size_t i = 0; i < format.size(); )
    {
        wchar_t c = format[i];

        if (c == L'{')
        {
            if (i + 1 < format.size() && format[i + 1] == L'{') { out += L'{'; i += 2; continue; }

            size_t j = i + 1;
            if (j >= format.size() || !iswdigit(format[j])) return format;

            long v = 0;
            while (j < format.size() && iswdigit(format[j]))
            {
                v = v * 10 + (format[j] - L'0');
                ++j;
            }
            if (j >= format.size() || format[j] != L'}' || v >= requiredParams) return format;

            out += (v < (long)g_extraArgs.size()) ? g_extraArgs[v] : std::wstring(L" ");
            i = j + 1;
            continue;
        }

        if (c == L'}')
        {
            if (i + 1 < format.size() && format[i + 1] == L'}') { out += L'}'; i += 2; continue; }
            return format;
        }

        out += c;
        ++i;
    }
    return out;
}

// ============================================================================
// 路径处理（对照 C# ProcessPath）
// ============================================================================

static std::wstring ProcessPath(const std::wstring& upathIn, std::wstring parentDirectory)
{
    if (upathIn.empty()) return upathIn;

    std::wstring upath = upathIn;

    // [Config] 别名替换（仅一层）
    std::wstring alias;
    if (ReadValue(L"Config", upath, alias) && !alias.empty())
        upath = alias;

    // 盘符绝对路径：直接返回
    if (upath.size() > 3 && upath[1] == L':' && upath[2] == L'\\')
        return upath;

    // 标记词开头的命令不做路径解析
    if (StartsWithNoCase(upath, L"cmd ") || StartsWithNoCase(upath, L"ps ") ||
        StartsWithNoCase(upath, L"powershell ") || StartsWithNoCase(upath, L"runadmin ") ||
        StartsWithNoCase(upath, L"show "))
        return upath;

    // 裸命令词：按 PATH 查找
    if (upath.find_first_of(L"\\/:") == std::wstring::npos && GetExtension(upath).empty())
        return upath;

    if (StartsWithNoCase(upath, L"http"))
        return upath;

    // 去掉开头反斜杠
    if (!upath.empty() && upath[0] == L'\\')
        upath.erase(upath.begin());

    if (StartsWithNoCase(upath, L"AppData"))
    {
        upath = upath.substr(7);
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
        {
            // 对照 Directory.GetParent(SpecialFolder.ApplicationData)：
            // C:\Users\<user>\AppData\Roaming 的父目录 = C:\Users\<user>\AppData
            std::wstring ad = appdata;
            size_t sep = ad.find_last_of(L'\\');
            if (sep != std::wstring::npos)
                parentDirectory = ad.substr(0, sep);
        }
    }
    else if (StartsWithNoCase(upath, L"..\\"))
    {
        while (StartsWithNoCase(upath, L"..\\"))
        {
            parentDirectory = GetParentDir(parentDirectory);
            upath = upath.substr(3);
        }
    }
    else if (StartsWithNoCase(upath, L"../"))
    {
        while (StartsWithNoCase(upath, L"../"))
        {
            parentDirectory = GetParentDir(parentDirectory);
            upath = upath.substr(3);
        }
    }
    else if (StartsWithNoCase(upath, L"pf\\"))
    {
        upath = upath.substr(3);
        for (wchar_t c = L'C'; c <= L'G'; ++c)
        {
            parentDirectory = std::wstring(1, c) + L":\\Program Files\\";
            if (FileExists(CombinePath(parentDirectory, upath)))
            {
                upath = CombinePath(parentDirectory, upath);
                break;
            }
        }
    }
    else if (StartsWithNoCase(upath, L"pf86\\"))
    {
        upath = upath.substr(5);
        for (wchar_t c = L'C'; c <= L'G'; ++c)
        {
            parentDirectory = std::wstring(1, c) + L":\\Program Files (x86)\\";
            if (FileExists(CombinePath(parentDirectory, upath)))
            {
                upath = CombinePath(parentDirectory, upath);
                break;
            }
        }
    }

    // 仍未成为盘符路径 → 与父目录拼接
    if (upath.size() > 3 && !(upath[1] == L':' && upath[2] == L'\\'))
    {
        while (!upath.empty() && upath[0] == L'\\')
            upath.erase(upath.begin());
        upath = CombinePath(parentDirectory, upath);
    }
    return upath;
}

// ============================================================================
// 程序与参数分离（对照 C# ProcessString，findChar=false 分支）
// ============================================================================

static void ProcessString(const std::wstring& input, std::wstring& program, std::wstring& arguments)
{
    program.clear();
    arguments.clear();
    if (input.empty()) return;

    if (input[0] == L'"')
    {
        size_t close = input.find(L'"', 1);
        if (close == std::wstring::npos)
        {
            program = input.substr(1);
            return;
        }
        program = input.substr(1, close - 1);
        arguments = Trim(input.substr(close + 1));
        return;
    }

    // ".exe " 分割（忽略大小写）
    std::wstring lower = ToLower(input);
    size_t exePos = lower.find(L".exe ");
    if (exePos != std::wstring::npos)
    {
        program = input.substr(0, exePos + 4);
        arguments = Trim(input.substr(exePos + 5));
        return;
    }

    // 兜底：首个词为裸命令词时按首个空格分离
    size_t sp = input.find(L' ');
    if (sp != std::wstring::npos && sp > 0)
    {
        std::wstring head = input.substr(0, sp);
        if (head.find_first_of(L"\\/:") == std::wstring::npos)
        {
            program = head;
            arguments = Trim(input.substr(sp + 1));
            return;
        }
    }

    program = input;
}

// ============================================================================
// 进程启动（对照 C# StartProcess / CmdExec / PowerShellExec）
// ============================================================================

// ShellExecuteEx 启动（show 或提权场景）；workDir 为空表示继承当前目录
static bool ShellExec(const std::wstring& fileName, const std::wstring& arguments, bool runas,
                      const std::wstring& workDir)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = runas ? L"runas" : L"open";
    sei.lpFile = fileName.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

static bool HasWhitespace(const std::wstring& s)
{
    return s.find_first_of(L" \t") != std::wstring::npos;
}

// CreateProcess 启动（默认静默：不创建控制台窗口），对照 UseShellExecute=false + CreateNoWindow
static bool CreateProc(const std::wstring& fileName, const std::wstring& arguments, bool show,
                       const std::wstring& workDir)
{
    // 裸命令名（cmd.exe、robocopy…）不能作为 lpApplicationName 传（不走 PATH 搜索），
    // 交给命令行首词解析；带路径的名字直接作为 lpApplicationName。
    bool bareName = fileName.find_first_of(L"\\/:") == std::wstring::npos;

    std::wstring cmdLine = HasWhitespace(fileName) ? (L"\"" + fileName + L"\"") : fileName;
    if (!arguments.empty())
        cmdLine += L" " + arguments;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = show ? 0 : CREATE_NO_WINDOW;

    BOOL ok = CreateProcessW(bareName ? nullptr : fileName.c_str(), cmdLine.data(),
                             nullptr, nullptr, FALSE, flags, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 启动一个进程：带参数走 shell/静默；无参数的运行/打开逻辑对照 C#
static void StartProcess(const std::wstring& file, const std::wstring& arguments, bool runas, bool show)
{
    if (file.empty()) return;

    bool shell = show || runas;
    std::wstring fileName = file;
    std::wstring args = arguments;

    if (args.empty())
    {
        if (runas)
        {
            // 保持 fileName
        }
        else if (IsBareCommand(file))
        {
            // 保持 fileName
        }
        else
        {
            // 打开语义：目录 / 文档 / 网址交给 explorer
            fileName = L"explorer.exe";
            args = file;
        }
    }

    if (shell)
        ShellExec(fileName, args, runas, L"");
    else
        CreateProc(fileName, args, show, L"");
}

// cmd /c 执行（对照 C# CmdExec：默认无窗口，show 时可见；提权走 runas；工作目录＝exe 目录）
static void CmdExec(const std::wstring& command, bool runas, bool show)
{
    std::wstring args = L"/c " + command;
    bool ok = runas ? ShellExec(L"cmd.exe", args, true, g_exeDir)
                    : CreateProc(L"cmd.exe", args, show, g_exeDir);
    if (!ok)
        ShowMessageBox(L"CMD 启动失败：" + std::to_wstring(GetLastError()));
}

// PowerShell -EncodedCommand 执行（对照 C# PowerShellExec）
static std::wstring Base64Encode(const BYTE* data, size_t len)
{
    static const wchar_t* kTable = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < len)
    {
        DWORD n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += kTable[n & 63];
        i += 3;
    }
    if (i + 1 == len)
    {
        DWORD n = data[i] << 16;
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += L"==";
    }
    else if (i + 2 == len)
    {
        DWORD n = (data[i] << 16) | (data[i + 1] << 8);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += L'=';
    }
    return out;
}

static void PowerShellExec(const std::wstring& command, bool runas, bool show)
{
    if (command.empty()) return;
    // UTF-16LE 字节 → Base64（对照 Encoding.Unicode.GetBytes）
    std::wstring encoded = Base64Encode(reinterpret_cast<const BYTE*>(command.data()),
                                        command.size() * sizeof(wchar_t));
    std::wstring args = L"-NoLogo -NoProfile -EncodedCommand " + encoded;
    bool ok = runas ? ShellExec(L"powershell.exe", args, true, g_exeDir)
                    : CreateProc(L"powershell.exe", args, show, g_exeDir);
    if (!ok)
        ShowMessageBox(L"PowerShell 启动失败：" + std::to_wstring(GetLastError()));
}

// ============================================================================
// 执行入口（对照 C# WinExec）：占位符 → 标记词序列 → cmd/ps/直接启动
// ============================================================================

static void WinExec(std::wstring upath, bool runas = false)
{
    if (Trim(upath).empty()) return;

    // 全局占位符替换（{time.*}/{env.*}/{guid.*}/{random.*}），cmd / ps 命令同样生效
    upath = ProcessPlaceholders(upath);

    // {0}{1}… 参数填充（带参数运行分身或模板时）；值不含占位符时把参数追加到命令尾部
    int requiredParams = GetFormatParameterCount(upath);
    if (requiredParams > 0)
    {
        upath = FillFormatArgs(upath, requiredParams);
    }
    else if (g_hasExtraArgs && !g_extraArgs.empty())
    {
        upath += L" " + Join(g_extraArgs, L" ");
    }

    // 标记词序列解析：runadmin（管理员）/ show（显示窗口）/ cmd / ps / powershell，顺序任意
    std::wstring body = Trim(upath);
    std::wstring shell;
    bool show = false;

    for (;;)
    {
        size_t sp = body.find(L' ');
        std::wstring word = (sp == std::wstring::npos) ? body : body.substr(0, sp);

        if (EqualsNoCase(word, L"runadmin"))
            runas = true;
        else if (EqualsNoCase(word, L"show"))
            show = true;
        else if (EqualsNoCase(word, L"cmd"))
            shell = L"cmd";
        else if (EqualsNoCase(word, L"ps") || EqualsNoCase(word, L"powershell"))
            shell = L"ps";
        else
            break; // 首个非标记词即命令体开始

        if (sp == std::wstring::npos)
        {
            body.clear(); // 只有标记词，没有命令体
            break;
        }
        body = Trim(body.substr(sp + 1));
    }

    if (body.empty()) return;

    if (shell == L"cmd")
    {
        CmdExec(body, runas, show);
        return;
    }
    if (shell == L"ps")
    {
        PowerShellExec(body, runas, show);
        return;
    }

    std::wstring program, arguments;
    ProcessString(body, program, arguments);
    StartProcess(ProcessPath(program, g_parentDir), arguments, runas, show);
}

// ============================================================================
// 主流程（对照 C# NoArgs / CfgInit / RunRunme / ShowListBox / ReplaceAll…）
// ============================================================================

static void ShowListBox();

// 以 UTF-8 + BOM 写入（对照 File.WriteAllText(..., Encoding.UTF8)）
static bool WriteTextFileUtf8Bom(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string bytes = "\xEF\xBB\xBF";
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (len > 0)
    {
        size_t offset = bytes.size();
        bytes.resize(offset + (size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &bytes[offset], len, nullptr, nullptr);
    }

    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// 首启生成带注释的默认配置（对照 C# CfgInit）
static void CfgInit()
{
    if (FileExists(g_cfgPath)) return;

    // 生成带注释的默认配置：每个功能点一条示例，注释写在配置上方
    std::vector<std::wstring> lines = {
        L"# ================= RunMe 使用说明 ==================",
        L"# 改名即用：把 RunMe.exe 改名为入口名（如 Vs.exe），在 [Config] 中配置同名键，",
        L"# 双击分身（如 Vs.exe）即可启动对应程序；一个小 exe 可复制成任意多个“入口”。",
        L"#",
        L"# 占位符（所有配置值中可用）：",
        L"#   {time.格式}         当前时间，如 {time.yyyyMMdd}",
        L"#   {env.变量名}        引用 [Settings] 中的自定义变量",
        L"#   {guid.id}           生成新的 GUID",
        L"#   {random.最小-最大}  生成区间内随机整数（支持负数）",
        L"#   {0} {1} …           命令行参数；带参数运行分身（如拖拽文件到 exe 上）时自动填入",
        L"#",
        L"# 执行方式（写在命令开头；runadmin 与 cmd/ps 顺序任意，[Config] 值与列表条目均可用）：",
        L"#   cmd 命令            用 CMD 执行（可用重定向、管道等）",
        L"#   ps 命令             用 PowerShell 执行",
        L"#   runadmin            管理员权限（例：runadmin cmd xxx、cmd runadmin xxx、ps runadmin xxx）",
        L"#   show                显示窗口执行（默认不显示窗口；可与 cmd/ps 任意组合，例：show cmd xxx、cmd show xxx）",
        L"#   都不加              直接启动；裸命令（dotnet、git、notepad…）按系统 PATH 查找",
        L"",
        L"[Settings]",
        L"# 相对路径的基准目录（一般保持为程序所在目录）",
        L"RunParentDirectory=" + g_exeDir,
        L"# list 模式排除名单（| 分隔、不含扩展名；仅 list 生效，[Config] 列表不受影响）",
        L"ExcludeExeName=RunMe|MeRun",
        L"# 列表窗口倒计时秒数：到点自动启动当前选中项，标题栏显示剩余秒数",
        L"#   （用户一旦按键/滚轮/点击即取消倒计时；0 = 完全关闭自动启动）",
        L"ListAutoRunSeconds=5",
        L"# 自定义变量：供 {env.变量名} 引用",
        L"apiKey=你的密钥",
        L"",
        L"[Config]",
        L"# 键 = 分身 exe 名（不含 .exe）；值 = 启动目标（路径 / 命令 / 显示名|目标,… 列表）",
        L"",
        L"# ① 绝对路径：双击 Pg.exe 直接启动",
        L"Pg=C:\\Windows\\System32\\notepad.exe",
        L"",
        L"# ② 相对路径：基于 [Settings] RunParentDirectory 拼接",
        L"Rel=Tools\\SomeTool\\tool.exe",
        L"",
        L"# ③ 路径：绝对路径直接运行；相对路径前缀 pf\\ = C~G 盘 Program Files、pf86\\ = Program Files (x86)、",
        L"#    AppData = 用户目录、..\\ = 上一级目录，其余相对路径以 [Settings] RunParentDirectory 为基础拼接",
        L"Firefox=pf\\Mozilla Firefox\\firefox.exe",
        L"",
        L"# ④ 网址与目录：直接打开",
        L"Bing=https://www.bing.com",
        L"Docs=Docs\\手册",
        L"",
        L"# ⑤ 裸命令：按系统 PATH 查找（无需加 cmd）",
        L"IPConfig=ipconfig",
        L"",
        L"# ⑥ cmd 前缀：需要 CMD 特性（重定向、管道、start 等）时使用",
        L"WinCalc=cmd start \"\" shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
        L"",
        L"# ⑦ ps 前缀：用 PowerShell 执行",
        L"HelloPS=ps echo hello > \"$HOME\\hello.txt\"",
        L"",
        L"# ⑧ 多条目列表：值以 runme 开头 + 显示名|目标,…（单条直接启动，多条弹列表）",
        L"#    列表窗口：Enter 启动 / Shift+Enter 管理员启动 / 双击 / 滚轮 / 方向键循环 / Esc 关闭",
        L"#    倒计时到点自动启动当前选中项（标题栏显示剩余秒数；一旦操作即取消，见 [Settings] ListAutoRunSeconds）",
        L"Dev=runme 7-Zip|pf\\7-Zip\\7zFM.exe,Notepad++|pf\\Notepad++\\notepad++.exe",
        L"Tools=runme 记事本|notepad,计算器|cmd start calc",
        L"",
        L"# ⑨ 占位符：{time.*} {env.*} {guid.*} {random.*} 在执行时自动替换",
        L"Tmp=cmd echo {time.yyyyMMdd}-{random.1-99} > \"%TEMP%\\{guid.id}.txt\"",
        L"",
        L"# ⑩ 参数：带参数运行分身（如拖拽文件到 exe 上）时 {0} 自动填入",
        L"Deploy=dotnet publish {0} -c Release",
        L"",
        L"# ⑪ 批量启动：新建 {分身名}run.txt（如 Pgrun.txt），每行一个程序，双击分身按行依次启动",
        L"",
        L"# ⑫ 管理员权限：命令中带 runadmin 一词即管理员（与 cmd / ps 顺序任意），执行时弹 UAC 确认",
        L"Hosts=cmd runadmin notepad C:\\Windows\\System32\\drivers\\etc\\hosts",
        L"",
        L"# ================= 命令行命令（如 Vs.exe 后跟） ==================",
        L"#   help                    显示帮助",
        L"#   list 扩展名 [目录]      列出目录中指定后缀的文件供选择（目录缺省为本目录）",
        L"#   runme 显示名|目标,…    临时列表（单条直接启动，多条弹列表）",
        L"#   runmeth                 目录中其它 exe 全部替换为当前 exe",
        L"#   runmefth                按 [Config] 的键批量生成分身（已存在不覆盖）",
        L""
    };

    std::wstring text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i) text += L"\r\n";
        text += lines[i];
    }

    WriteTextFileUtf8Bom(g_cfgPath, text);
}

// 目录中按通配符枚举文件（非递归，仅文件）
static std::vector<std::wstring> ListFilesInDir(const std::wstring& dir, const std::wstring& filter)
{
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(CombinePath(dir, filter).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(CombinePath(dir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

// 按行切分（对照 StreamReader.ReadLine：\r\n / \n / \r；结尾分隔符不产生空行）
static std::vector<std::wstring> SplitLines(const std::wstring& text)
{
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (size_t i = 0; i < text.size(); ++i)
    {
        wchar_t c = text[i];
        if (c == L'\r' || c == L'\n')
        {
            lines.push_back(cur);
            cur.clear();
            if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// 逐行执行 {name}run.txt（首行立即，之后每行间隔 1 秒；对照 C# RunFileContent）
static void RunFileContent(const std::wstring& runFilePath)
{
    std::wstring text;
    if (!ReadFileUtf8(runFilePath, text)) return;

    bool first = true;
    for (auto& line : SplitLines(text))
    {
        if (!first) Sleep(1000);
        first = false;
        WinExec(line);
    }
}

// ---- runme 列表（对照 C# RunDict / RunRunme / ShowListBox） ----

struct RunEntry
{
    std::wstring name;
    std::wstring target;
};

static std::vector<RunEntry> g_runDict;   // 顺序即列表显示顺序（对照 Dictionary 插入顺序）
static HWND g_listBoxWnd = nullptr;
static WNDPROC g_listBoxProc = nullptr;

// 列表窗口倒计时（[Settings] ListAutoRunSeconds，默认 5 秒；0 = 不自动启动）
static const wchar_t* kListTitle = L"很牛B的一个程序启动器";
static const UINT_PTR kListTimerId = 0x9527;
static int g_listAutoRunSeconds = 5;
static int g_listCountdownLeft = 0;

static void RunDictSet(const std::wstring& name, const std::wstring& target)
{
    for (auto& e : g_runDict)
    {
        if (e.name == name)
        {
            e.target = target;      // 同名覆盖，保持首次出现的位置
            return;
        }
    }
    RunEntry entry;
    entry.name = name;
    entry.target = target;
    g_runDict.push_back(entry);
}

static void RunRunme(std::wstring args)
{
    if (args.empty()) return;

    // 兼容 "runme 名称|目标,…" 写法（剥离列表标记）
    if (StartsWithNoCase(args, L"runme "))
        args = TrimStartWs(args.substr(6));

    for (auto& se : Split(args, L',', true))
    {
        auto list2 = Split(se, L'|', true);
        if (list2.size() > 1)
            RunDictSet(list2[0], ProcessPath(list2[1], g_parentDir));
    }

    if (g_runDict.size() == 1)
        WinExec(g_runDict[0].target);
    else
        ShowListBox();   // 0 条时内部直接返回（窗体不再显示）
}

// 取指定目录指定后缀列表（对照 C# GetFilesList；名字在 [Settings] ExcludeExeName 中的跳过）
static void GetFilesList(const std::wstring& path, const std::wstring& suffix)
{
    if (TrimWs(path).empty() || !DirExists(path))
    {
        ShowMessageBox(L"目录不存在: " + path);
        return;
    }

    std::vector<std::wstring> excludes;
    std::wstring raw;
    if (ReadValue(L"Settings", L"ExcludeExeName", raw) && !raw.empty())
        excludes = Split(raw, L'|', true);
    if (excludes.empty())
    {
        excludes.push_back(L"RunMe");
        excludes.push_back(L"MeRun");
    }

    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(CombinePath(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)   // 空目录不算错误
            ShowMessageBox(L"读取目录失败: " + path);
        return;
    }

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring name = fd.cFileName;
        if (!EqualsNoCase(GetExtension(name), suffix)) continue;

        std::wstring base = RemoveExtension(name);
        bool skip = false;
        for (auto& x : excludes)
        {
            if (EqualsNoCase(x, base)) { skip = true; break; }
        }
        if (!skip) names.push_back(base);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    for (auto& n : names)
        RunDictSet(n, CombinePath(path, n + suffix));
}

// 启动指定条目（对照 Enter / 双击 / 倒计时到期）
static void ListLaunchIndex(int index, bool runas)
{
    if (!g_listBoxWnd) return;
    if (index < 0 || index >= (int)g_runDict.size()) return;

    std::wstring target = g_runDict[index].target;
    HWND main = GetParent(g_listBoxWnd);

    WinExec(target, runas);     // 对照 C#：先 WinExec 再 Close()
    DestroyWindow(main);
}

// 启动当前选中项（对照 Enter / 双击）
static void ListLaunchSelected(bool runas)
{
    if (!g_listBoxWnd) return;

    ListLaunchIndex((int)SendMessageW(g_listBoxWnd, LB_GETCURSEL, 0, 0), runas);
}

// 标题栏文字：原始标题 + 倒计时（倒计时已关闭/已取消时不加后缀）
static std::wstring MakeListTitle(int sel)
{
    std::wstring title = kListTitle;
    if (g_listAutoRunSeconds > 0 && g_listCountdownLeft > 0 && !g_runDict.empty())
    {
        if (sel < 0 || sel >= (int)g_runDict.size()) sel = 0;
        title += L"（" + std::to_wstring(g_listCountdownLeft) + L" 秒后启动 " + g_runDict[sel].name + L"）";
    }
    return title;
}

// 量出标题栏文字宽度（用于把窗口撑到放得下标题）
static int MeasureTitleWidth(const std::wstring& title)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return 0;

    HDC hdc = GetDC(nullptr);
    if (!hdc) return 0;

    HFONT font = CreateFontIndirectW(&ncm.lfCaptionFont);
    HFONT old = (HFONT)SelectObject(hdc, font);

    SIZE textSize{};
    GetTextExtentPoint32W(hdc, title.c_str(), (int)title.size(), &textSize);

    SelectObject(hdc, old);
    if (font) DeleteObject(font);
    ReleaseDC(nullptr, hdc);

    return textSize.cx;
}

// 标题栏：刷新为“原始标题 + 倒计时”（倒计时取消后自动回到原始标题）
static void ListUpdateTitle()
{
    if (!g_listBoxWnd || !IsWindow(g_listBoxWnd)) return;

    HWND main = GetParent(g_listBoxWnd);
    if (!main) return;

    SetWindowTextW(main, MakeListTitle((int)SendMessageW(g_listBoxWnd, LB_GETCURSEL, 0, 0)).c_str());
}

// 用户开始操作（按键 / 滚轮 / 点击）→ 取消倒计时：不再自动启动，标题栏也不再显示
static void ListCancelCountdown()
{
    if (g_listCountdownLeft <= 0) return;

    g_listCountdownLeft = 0;

    HWND main = (g_listBoxWnd && IsWindow(g_listBoxWnd)) ? GetParent(g_listBoxWnd) : nullptr;
    if (main) KillTimer(main, kListTimerId);

    ListUpdateTitle();
}

// 列表框子类化：Enter / Shift+Enter / 方向键循环 / 滚轮循环切换 / Esc（对照 C# 键盘与滚轮事件）
static LRESULT CALLBACK ListBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    if (msg == WM_KEYDOWN)
    {
        if (wp == VK_RETURN)
        {
            bool runas = (GetKeyState(VK_SHIFT) & 0x8000) != 0;   // Shift+Enter 管理员启动
            ListLaunchSelected(runas);
            return 0;
        }
        if (wp == VK_ESCAPE)
        {
            DestroyWindow(GetParent(hwnd));
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN)
        {
            // 方向键循环：末项按下回到首项，首项按上跳到末项
            int count = (int)SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            if (count <= 0) return 0;

            int cur = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
            if (cur < 0 || cur >= count) cur = 0;

            int next = (wp == VK_DOWN) ? ((cur + 1) % count) : ((cur + count - 1) % count);
            SendMessageW(hwnd, LB_SETCURSEL, next, 0);
            SendMessageW(hwnd, LB_SETCARETINDEX, next, FALSE);
            ListCancelCountdown();       // 一旦动手选择，倒计时就取消
            return 0;
        }
    }
    else if (msg == WM_MOUSEWHEEL)
    {
        int count = (int)SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
        if (count <= 0) return 0;

        int cur = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        if (cur < 0)
        {
            SendMessageW(hwnd, LB_SETCURSEL, 0, 0);
            ListCancelCountdown();
            return 0;
        }

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int next = cur;
        if (delta > 0)
            next = (cur <= 0) ? count - 1 : cur - 1;
        else if (delta < 0)
            next = (cur >= count - 1) ? 0 : cur + 1;

        SendMessageW(hwnd, LB_SETCURSEL, next, 0);
        SendMessageW(hwnd, LB_SETCARETINDEX, next, FALSE);
        ListCancelCountdown();           // 滚轮也是“动手选择”
        return 0;
    }

    // 其余按键 / 点击先取消倒计时，再走默认处理（默认处理可能改选中项，之后再刷新标题）
    if (msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
        ListCancelCountdown();

    LRESULT result = CallWindowProcW(g_listBoxProc, hwnd, msg, wp, lp);

    if (IsWindow(hwnd) && (msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN))
        ListUpdateTitle();

    return result;
}

// 列表窗口过程（自绘条目 + 双击启动）
static LRESULT CALLBACK ListWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == kListTimerId)
        {
            if (--g_listCountdownLeft <= 0)
            {
                KillTimer(hwnd, kListTimerId);

                // 倒计时到期：启动当前选中项（未操作时即第一项，即“默认启动第一个”）
                int sel = g_listBoxWnd ? (int)SendMessageW(g_listBoxWnd, LB_GETCURSEL, 0, 0) : 0;
                ListLaunchIndex(sel < 0 ? 0 : sel, false);
            }
            else
            {
                ListUpdateTitle();
            }
            return 0;
        }
        break;

    case WM_SIZE:
        if (g_listBoxWnd)
            MoveWindow(g_listBoxWnd, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        if (dis->CtlType != ODT_LISTBOX) break;

        bool sel = (dis->itemState & ODS_SELECTED) != 0;
        FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));

        if (dis->itemID != (UINT)-1)
        {
            wchar_t buf[512];
            int n = (int)SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)buf);
            if (n > 0)
            {
                HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
                HFONT old = (HFONT)SelectObject(dis->hDC, font);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));

                RECT r = dis->rcItem;
                r.left += 3;
                DrawTextW(dis->hDC, buf, n, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                SelectObject(dis->hDC, old);
            }
        }
        return TRUE;
    }

    case WM_COMMAND:
        if ((HWND)lp == g_listBoxWnd && HIWORD(wp) == LBN_DBLCLK)
        {
            ListLaunchSelected(false);      // 双击启动（对照 ListBox1_DoubleClick）
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 显示列表窗口（对照 C# ShowListBox；无条目时不显示窗口）
static void ShowListBox()
{
    if (g_runDict.size() < 1) return;

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ListWndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"RunMeListWnd";
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }

    const int formWidth = 402;      // 对照 FormWidth
    const int itemHeight = 48;      // 对照 ItemHeight
    int height = itemHeight + itemHeight * (int)g_runDict.size();
    if (height > 800) height = 800; // 对照 MaxFormHeight

    // 倒计时会让标题变长：先按标题文字宽度把窗口撑宽（不小于原宽度，且不超过工作区）
    g_listCountdownLeft = g_listAutoRunSeconds;
    int listWidth = formWidth;
    if (g_listAutoRunSeconds > 0)
    {
        int needed = MeasureTitleWidth(MakeListTitle(0)) + 190;   // 190 ≈ 图标 + 最小化/关闭按钮 + 边框
        if (needed > listWidth) listWidth = needed;
    }

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (listWidth > wa.right - wa.left) listWidth = wa.right - wa.left;
    int x = wa.left + ((wa.right - wa.left) - listWidth) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - height) / 2;

    HWND hwnd = CreateWindowExW(0, L"RunMeListWnd", MakeListTitle(0).c_str(), WS_OVERLAPPEDWINDOW,
                                x, y, listWidth, height, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return;

    g_listBoxWnd = CreateWindowExW(
        0, L"ListBox", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,
        0, 0, listWidth, height, hwnd, (HMENU)1, hinst, nullptr);
    if (!g_listBoxWnd)
    {
        DestroyWindow(hwnd);
        return;
    }

    // 对照 Font("微软雅黑", 26.25f)（26.25pt ≈ 35px @96dpi）
    HFONT font = CreateFontW(-35, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
    SendMessageW(g_listBoxWnd, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_listBoxWnd, LB_SETITEMHEIGHT, 0, (LPARAM)itemHeight);

    for (auto& e : g_runDict)
        SendMessageW(g_listBoxWnd, LB_ADDSTRING, 0, (LPARAM)e.name.c_str());

    g_listBoxProc = (WNDPROC)SetWindowLongPtrW(g_listBoxWnd, GWLP_WNDPROC, (LONG_PTR)ListBoxProc);

    SendMessageW(g_listBoxWnd, LB_SETCURSEL, 0, 0);      // 默认选中第一项
    SendMessageW(g_listBoxWnd, LB_SETCARETINDEX, 0, FALSE);

    // 倒计时：到点自动启动当前选中项（用户一旦操作即取消，见 ListCancelCountdown）
    if (g_listAutoRunSeconds > 0)
        SetTimer(hwnd, kListTimerId, 1000, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(g_listBoxWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_listBoxWnd = nullptr;
    g_listCountdownLeft = 0;
    if (font) DeleteObject(font);
}

// runmeth：目录中其它 exe 全部替换为当前 exe（对照 C# ReplaceAll）
static void ReplaceAll()
{
    for (auto& se : ListFilesInDir(g_exeDir, L"*.exe"))
    {
        if (EqualsNoCase(se, g_exePath)) continue;   // 跳过自身
        DeleteFileW(se.c_str());                     // File.Delete + File.Copy（失败跳过该文件）
        CopyFileW(g_exePath.c_str(), se.c_str(), FALSE);
    }
}

// runmefth：先清掉非自身的 exe，再按 [Config] 的键批量生成分身（对照 C# ReplaceAllX）
static void ReplaceAllX()
{
    for (auto& se : ListFilesInDir(g_exeDir, L"*.exe"))
    {
        if (EqualsNoCase(RemoveExtension(GetFileName(se)), g_exeName)) continue;
        DeleteFileW(se.c_str());
    }

    for (auto& section : g_config)
    {
        if (EqualsNoCase(section.first, L"Settings")) continue;

        for (auto& kv : section.second)
        {
            if (EqualsNoCase(kv.first, g_exeName)) continue;

            std::wstring target = CombinePath(g_exeDir, kv.first + L".exe");
            if (!FileExists(target))
                CopyFileW(g_exePath.c_str(), target.c_str(), FALSE);
        }
    }
}

// 显示帮助信息（对照 C# ShowMessage）
static void ShowMessage()
{
    std::vector<std::wstring> lines = {
        L"改名即用：把本 exe 改名为入口名（如 Vs.exe），在 YanBinCfg.ini 的 [Config] 中配置同名键。",
        L"",
        L"  Vs=程序或路径                       双击直接启动",
        L"  Dev=runme 名称1|目标1,名称2|目标2   双击弹出列表选择（runme 是列表标记）",
        L"",
        L"值支持的写法（配置文件、列表条目目标、run.txt 行通用）：",
        L"  · cmd / ps / powershell 命令         用 CMD 或 PowerShell 执行",
        L"  · runadmin 目标                      管理员权限（与 cmd / ps 顺序任意：cmd runadmin xxx、ps runadmin xxx）",
        L"  · show 目标                          显示窗口运行（默认不显示，例：show cmd xxx、cmd show xxx；提权时总显示）",
        L"  · 裸命令                             dotnet、git、notepad 等按系统 PATH 直接执行",
        L"  · 网址 / 目录 / 文件                 https://…、目录、文档交给系统默认方式打开",
        L"  · 占位符                             {time.格式} {env.变量名} {guid.id} {random.最小-最大}",
        L"  · {0}{1}…                            带参数运行分身（如拖拽文件到 exe 上）时自动填入",
        L"  · 路径                               绝对路径直接运行；相对路径支持 pf\\、pf86\\、AppData、..\\ 前缀，其余按基准目录拼接",
        L"",
        L"列表窗口：Enter 启动 / Shift+Enter 管理员启动 / 双击启动 / 滚轮切换 / 方向键循环 / Esc 关闭",
        L"          倒计时到点自动启动当前选中项（标题栏显示剩余秒数；按键/滚轮/点击即取消）",
        L"",
        L"批量启动：新建 {分身名}run.txt（如 Vsrun.txt），每行一个目标，双击分身按行依次启动",
        L"",
        L"配置：[Settings] RunParentDirectory=相对路径基准目录；ExcludeExeName=list 模式的排除名单；",
        L"      ListAutoRunSeconds=列表窗口倒计时秒数（默认 5，0=不自动启动）",
        L"",
        L"其他命令（命令行传递）：",
        L"  runme 显示名|目标,…    临时列表（单条直接启动，多条弹列表）",
        L"  list 扩展名 [目录]     列出目录中指定后缀的文件供选择（目录缺省为本目录）",
        L"  runmeth                目录中其它 exe 全部替换为当前 exe",
        L"  runmefth               按 [Config] 的键批量生成分身（已存在不覆盖）",
        L"  help                   显示本帮助",
        L"",
        L"注：值以 runme 开头即为列表（显示名|目标,…），不带则整条按单条命令执行；详细说明见 YanBinCfg.ini 注释与 README.md"
    };

    ShowMessageBox(Join(lines, L"\r\n"), L"使用帮助");
}

// 无参数（或带参数落到自身配置）：{name}run.txt → [Config][name] → runme 列表/单条
static void NoArgs(std::wstring rname = L"")
{
    if (rname.empty()) rname = g_exeName;

    std::wstring runTxt = CombinePath(g_exeDir, rname + L"run.txt");
    if (FileExists(runTxt))
    {
        RunFileContent(runTxt);
        return;
    }

    std::wstring upath;
    if (!ReadValue(L"Config", rname, upath) || upath.empty()) return;

    // 列表标记：值以 "runme " 开头 → 列表（单条直通 / 多条弹窗）
    if (StartsWithNoCase(upath, L"runme "))
        RunRunme(TrimStartWs(upath.substr(6)));
    else
        WinExec(upath);
}

static void InitPaths()
{
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    std::wstring exePath(buf, n);
    g_exePath = exePath;
    g_exeDir = GetDirectory(exePath);
    g_exeName = RemoveExtension(GetFileName(exePath));
    g_cfgPath = CombinePath(g_exeDir, L"YanBinCfg.ini");
}

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    InitPaths();
    if (g_exeDir.empty() || g_exeName.empty()) return 0;

    // 对照 C# 顺序：FormInit → CfgInit（首启生成模板）→ InitializeConfigCache
    CfgInit();

    // 读取配置
    std::wstring cfgText;
    if (ReadFileUtf8(g_cfgPath, cfgText))
        ParseConfigText(cfgText, g_config);

    ReadValue(L"Settings", L"RunParentDirectory", g_parentDir);
    if (g_parentDir.empty() || g_exeDir.empty()) return 0;

    // 列表窗口倒计时秒数（[Settings] ListAutoRunSeconds，缺省 5 秒；0 = 不自动启动）
    std::wstring autoRunSeconds;
    if (ReadValue(L"Settings", L"ListAutoRunSeconds", autoRunSeconds))
    {
        int seconds = ParseIntOrDefault(autoRunSeconds, g_listAutoRunSeconds);
        g_listAutoRunSeconds = seconds > 0 ? seconds : 0;
    }

    // 命令行参数（argv[0] 为 exe 路径）
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);

    if (args.empty())
    {
        NoArgs();
        return 0;
    }

    if (StartsWithNoCase(args[0], L"runmeth"))
    {
        ReplaceAll();
        return 0;
    }

    if (StartsWithNoCase(args[0], L"runmefth"))
    {
        ReplaceAllX();
        return 0;
    }

    if (EqualsNoCase(args[0], L"runme") || StartsWithNoCase(args[0], L"runme "))
    {
        std::vector<std::wstring> cmdArgs(args.begin() + 1, args.end());
        if (args[0].size() > 5)
        {
            // 兼容 "runme xxx" 引号整串写法
            std::wstring inlineArg = TrimWs(args[0].substr(6));
            if (!inlineArg.empty())
                cmdArgs.insert(cmdArgs.begin(), inlineArg);
        }

        if (cmdArgs.empty()) return 0;
        RunRunme(Join(cmdArgs, L" "));
        return 0;
    }

    if (EqualsNoCase(args[0], L"list") || StartsWithNoCase(args[0], L"list "))
    {
        std::vector<std::wstring> listArgs;
        if (args[0].size() > 4)
        {
            for (auto& piece : Split(args[0].substr(5), L' ', true))
                listArgs.push_back(piece);
        }
        for (size_t i = 1; i < args.size(); ++i)
            listArgs.push_back(args[i]);

        // 至少要指定扩展名；目录可省略，默认取程序所在目录
        if (listArgs.size() < 1) return 0;
        std::wstring ext = TrimStartChar(listArgs[0], L'.');
        std::wstring dir = listArgs.size() > 1 ? listArgs[1] : g_exeDir;
        GetFilesList(dir, L"." + ext);
        ShowListBox();
        return 0;
    }

    if (EqualsNoCase(args[0], L"help"))
    {
        ShowMessage();
        return 0;
    }

    // 分身带参数运行（如拖拽文件到 exe 上）：执行自身配置，参数供 {0} 填充或追加到命令尾部
    std::wstring selfCfg;
    if (ReadValue(L"Config", g_exeName, selfCfg) && !selfCfg.empty())
    {
        g_extraArgs = args;
        g_hasExtraArgs = true;
        NoArgs();
        return 0;
    }

    // 兜底：首个参数当作"程序名/配置键"
    NoArgs(args[0]);
    return 0;
}
