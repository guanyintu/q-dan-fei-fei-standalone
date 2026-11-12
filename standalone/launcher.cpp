#include "protocol.hpp"
#include <CommCtrl.h>
#include <commdlg.h>
#include <TlHelp32.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <new>
#include "ui_theme.hpp"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr UINT AudioEvent = WM_APP + 2;
enum Control : int { Processes = 100, Refresh, Connect, Enable, Amp, Period, Random,
    ScopeSelf, ScopePlayers, ScopeNpcs, ScopeMonsters, Bgm, Muted, Volume, Path, Browse, ApplyMusic, Reset, Hotkeys,
    Status, Error, Version, AmpText, PeriodText, VolumeText, Credits };
HWND window;
HFONT font;
HINSTANCE instance;
fs::path dataDirectory, modulePath, defaultMusic, iniPath;
std::vector<std::pair<DWORD, std::wstring>> processes;
std::wstring versionWarning, audioError;
HANDLE gameProcess, mapHandle, moduleThread;
qb::Shared* shared;
DWORD gamePid;
bool stopping, closing;
bool desiredAudio, audioPlaying;
LONG audioReset;
std::wstring audioSource;
ComPtr<IMFMediaEngine> media;
bool mfStarted;

HWND Item(int id) { return GetDlgItem(window, id); }
void Text(int id, const std::wstring& value) {
    auto handle = Item(id); if (!handle) return;
    int length = GetWindowTextLengthW(handle);
    if (length == static_cast<int>(value.size())) {
        std::wstring previous(length + 1, L'\0'); GetWindowTextW(handle, previous.data(), length + 1); previous.resize(length);
        if (previous == value) return;
    }
    SetWindowTextW(handle, value.c_str());
}
std::wstring Value(int id) {
    auto count = GetWindowTextLengthW(Item(id)); std::wstring value(count + 1, L'\0');
    GetWindowTextW(Item(id), value.data(), count + 1); value.resize(count); return value;
}
bool Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void Check(int id, bool state) { if (Checked(id) != state) SendMessageW(Item(id), BM_SETCHECK, state ? BST_CHECKED : BST_UNCHECKED, 0); }
LONG Slider(int id) { return static_cast<LONG>(SendMessageW(Item(id), TBM_GETPOS, 0, 0)); }
void SetSlider(int id, LONG value) { if (Slider(id) != value) SendMessageW(Item(id), TBM_SETPOS, TRUE, value); }
void ShowError(const std::wstring& message) { Text(Error, message); }
std::wstring WinError(DWORD error) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"Windows 操作失败"; if (text) LocalFree(text); return result;
}
void Require(bool ok, const wchar_t* action) {
    if (!ok) throw std::wstring(action) + L"：" + WinError(GetLastError());
}
uint64_t Hash(const BYTE* data, DWORD count) {
    uint64_t value = 14695981039346656037ULL;
    for (DWORD i = 0; i < count; ++i) { value ^= data[i]; value *= 1099511628211ULL; } return value;
}
fs::path Extract(int resource, const wchar_t* base, const wchar_t* extension) {
    auto handle = FindResourceW(instance, MAKEINTRESOURCEW(resource), RT_RCDATA);
    Require(handle != nullptr, L"找不到内嵌资源");
    auto length = SizeofResource(instance, handle);
    auto bytes = static_cast<const BYTE*>(LockResource(LoadResource(instance, handle)));
    Require(bytes != nullptr && length > 0, L"读取内嵌资源失败");
    wchar_t suffix[32]{}; swprintf_s(suffix, L"-%016llx", Hash(bytes, length));
    auto path = dataDirectory / (std::wstring(base) + suffix + extension);
    if (fs::exists(path) && fs::file_size(path) == length) return path;
    auto temp = path; temp += L".tmp";
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(reinterpret_cast<const char*>(bytes), length)) throw std::wstring(L"无法写入资源缓存。");
    stream.close();
    Require(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), L"保存资源缓存失败");
    return path;
}
std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t text[32768]{}; DWORD count = 32768;
    bool ok = QueryFullProcessImageNameW(process, 0, text, &count) != FALSE; CloseHandle(process);
    return ok ? std::wstring(text, count) : L"";
}
void RefreshProcesses() {
    processes.clear(); SendMessageW(Item(Processes), CB_RESETCONTENT, 0, 0);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"ffxiv_dx11.exe")) continue;
        auto path = ProcessPath(entry.th32ProcessID); if (path.empty()) continue;
        processes.emplace_back(entry.th32ProcessID, path);
        auto label = L"ffxiv_dx11.exe  PID " + std::to_wstring(entry.th32ProcessID) + L"  " + path;
        SendMessageW(Item(Processes), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    CloseHandle(snapshot); if (!processes.empty()) SendMessageW(Item(Processes), CB_SETCURSEL, 0, 0);
}
uintptr_t RemoteModule(DWORD pid, const std::wstring& name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry); uintptr_t address = 0;
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
        if (!_wcsicmp(entry.szModule, name.c_str())) { address = reinterpret_cast<uintptr_t>(entry.modBaseAddr); break; }
    CloseHandle(snapshot); return address;
}
uintptr_t RemoteLoadLibrary(DWORD pid) {
    auto fn = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    MEMORY_BASIC_INFORMATION info{}; Require(VirtualQuery(reinterpret_cast<void*>(fn), &info, sizeof(info)) != 0, L"定位系统加载函数失败");
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH);
    auto base = RemoteModule(pid, fs::path(path).filename().wstring());
    if (!base) throw std::wstring(L"目标进程的系统加载模块无法定位。");
    return base + reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(info.AllocationBase);
}
DWORD ExportRva(const fs::path& path, const char* name) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 4096) throw std::wstring(L"内嵌模块格式无效。");
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    auto raw = [&](DWORD rva) -> char* {
        auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (rva >= section[i].VirtualAddress && rva < section[i].VirtualAddress + section[i].SizeOfRawData) {
                auto offset = section[i].PointerToRawData + rva - section[i].VirtualAddress;
                if (offset >= bytes.size()) throw std::wstring(L"模块导出表越界。");
                return bytes.data() + offset;
            }
        throw std::wstring(L"模块导出 RVA 无效。");
    };
    auto directory = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(raw(nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress));
    auto names = reinterpret_cast<DWORD*>(raw(directory->AddressOfNames));
    auto ordinals = reinterpret_cast<WORD*>(raw(directory->AddressOfNameOrdinals));
    auto functions = reinterpret_cast<DWORD*>(raw(directory->AddressOfFunctions));
    for (DWORD i = 0; i < directory->NumberOfNames; ++i) if (!strcmp(raw(names[i]), name)) return functions[ordinals[i]];
    throw std::wstring(L"模块启动导出函数不存在。");
}
void SaveSettings() {
    auto save = [](const wchar_t* key, int value) { auto text = std::to_wstring(value); WritePrivateProfileStringW(L"QBounce", key, text.c_str(), iniPath.c_str()); };
    save(L"Amplitude", Slider(Amp)); save(L"Period", Slider(Period)); save(L"Random", Checked(Random));
    save(L"Self", Checked(ScopeSelf)); save(L"Players", Checked(ScopePlayers)); save(L"Npcs", Checked(ScopeNpcs)); save(L"Monsters", Checked(ScopeMonsters));
    save(L"Bgm", Checked(Bgm)); save(L"Muted", Checked(Muted)); save(L"Volume", Slider(Volume)); save(L"Hotkeys", Checked(Hotkeys));
    auto path = Value(Path); WritePrivateProfileStringW(L"QBounce", L"AudioPath", path.c_str(), iniPath.c_str());
}
void PublishSettings() {
    if (!shared) return;
    qb::Set(shared->enabled, qb::MayEnable(qb::Read(shared->stage)) && Checked(Enable)); qb::Set(shared->amplitude, Slider(Amp)); qb::Set(shared->period, Slider(Period));
    qb::Set(shared->random, Checked(Random)); qb::Set(shared->hotkeys, Checked(Hotkeys));
    qb::Set(shared->scopes, (Checked(ScopeSelf) ? qb::Self : 0) | (Checked(ScopePlayers) ? qb::Players : 0) |
        (Checked(ScopeNpcs) ? qb::Npcs : 0) | (Checked(ScopeMonsters) ? qb::Monsters : 0));
    qb::Set(shared->bgm, Checked(Bgm)); qb::Set(shared->muted, Checked(Muted)); qb::Set(shared->volume, Slider(Volume));
}
void Labels() {
    wchar_t value[48]{}; swprintf_s(value, L"强度 %.2f", Slider(Amp) / 1000.0); Text(AmpText, value);
    swprintf_s(value, L"周期 %.2f 秒", Slider(Period) / 1000.0); Text(PeriodText, value);
    swprintf_s(value, L"音量 %ld%%", Slider(Volume) / 10); Text(VolumeText, value);
}
void ReleaseConnection() {
    if (shared) UnmapViewOfFile(shared); shared = nullptr;
    if (mapHandle) CloseHandle(mapHandle); mapHandle = nullptr;
    if (moduleThread) CloseHandle(moduleThread); moduleThread = nullptr;
    if (gameProcess) CloseHandle(gameProcess); gameProcess = nullptr;
    gamePid = 0; stopping = false; Check(Enable, false); EnableWindow(Item(Enable), FALSE); Text(Connect, L"连接游戏");
    qbui::SetBadge(0);
}
void ConnectGame() {
    int selected = static_cast<int>(SendMessageW(Item(Processes), CB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(processes.size())) throw std::wstring(L"请先启动 FF14 并刷新进程列表。");
    gamePid = processes[selected].first;
    std::wifstream versionFile(fs::path(processes[selected].second).parent_path() / L"ffxivgame.ver");
    std::wstring version; std::getline(versionFile, version);
    versionWarning = version == qb::ReferenceVersion ? L"版本与参考客户端一致；仍会校验关键函数。" :
        L"警告：当前版本 " + (version.empty() ? L"未知" : version) + L" 未经验证。仅关键入口或结构校验失败时停止。";
    Text(Version, versionWarning);
    gameProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, gamePid);
    Require(gameProcess != nullptr, L"无法打开游戏进程");
    wchar_t name[96]{}; qb::MappingName(name, 96, gamePid);
    mapHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(qb::Shared), name);
    auto existed = GetLastError() == ERROR_ALREADY_EXISTS;
    Require(mapHandle != nullptr, L"创建本地控制通道失败");
    shared = static_cast<qb::Shared*>(MapViewOfFile(mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(qb::Shared)));
    Require(shared != nullptr, L"映射本地控制通道失败");
    if (existed && shared->magic == qb::Magic && GetTickCount64() - static_cast<ULONGLONG>(qb::Heartbeat(*shared)) < 3000 &&
        qb::Read(shared->stage) != qb::Stopped && qb::Read(shared->stage) != qb::Failed)
        throw std::wstring(L"另一个 q弹狒狒 独立版已连接此客户端。");
    new (shared) qb::Shared; shared->controllerPid = GetCurrentProcessId();
    InterlockedExchange64(&shared->heartbeat, static_cast<LONG64>(GetTickCount64())); PublishSettings();
    auto remoteBase = RemoteModule(gamePid, modulePath.filename().wstring());
    if (!remoteBase) {
        auto pathBytes = (modulePath.wstring().size() + 1) * sizeof(wchar_t);
        void* remotePath = VirtualAllocEx(gameProcess, nullptr, pathBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Require(remotePath != nullptr, L"准备模块路径失败");
        if (!WriteProcessMemory(gameProcess, remotePath, modulePath.c_str(), pathBytes, nullptr)) {
            auto code = GetLastError(); VirtualFreeEx(gameProcess, remotePath, 0, MEM_RELEASE); SetLastError(code); Require(false, L"写入模块路径失败");
        }
        HANDLE loader = CreateRemoteThread(gameProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(RemoteLoadLibrary(gamePid)), remotePath, 0, nullptr);
        if (!loader) { auto code = GetLastError(); VirtualFreeEx(gameProcess, remotePath, 0, MEM_RELEASE); SetLastError(code); Require(false, L"加载自身模块失败"); }
        auto wait = WaitForSingleObject(loader, 5000); CloseHandle(loader);
        if (wait != WAIT_OBJECT_0) throw std::wstring(L"模块加载超时；为避免竞态，临时路径保留到游戏退出。");
        VirtualFreeEx(gameProcess, remotePath, 0, MEM_RELEASE);
        remoteBase = RemoteModule(gamePid, modulePath.filename().wstring());
        if (!remoteBase) throw std::wstring(L"系统未能加载 q弹狒狒 自身模块。");
    }
    moduleThread = CreateRemoteThread(gameProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + ExportRva(modulePath, "QBounceStart")), nullptr, 0, nullptr);
    Require(moduleThread != nullptr, L"启动自身模块失败");
    auto start = GetTickCount64();
    while (qb::Read(shared->stage) == qb::Initializing && GetTickCount64() - start < 5000) {
        InterlockedExchange64(&shared->heartbeat, static_cast<LONG64>(GetTickCount64())); Sleep(10);
    }
    if (qb::Read(shared->stage) != qb::Ready) {
        std::wstring error = shared->error[0] ? shared->error : L"模块初始化超时，效果未开启。";
        qb::Set(shared->shutdown, 1); throw error;
    }
    EnableWindow(Item(Enable), TRUE); Text(Connect, L"断开连接"); ShowError(L"");
    qbui::SetBadge(1);
}

class Notify final : public IMFMediaEngineNotify {
    volatile LONG references = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) { *value = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK; }
        *value = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override { auto count = InterlockedDecrement(&references); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY || event == MF_MEDIA_ENGINE_EVENT_ERROR) PostMessageW(window, AudioEvent, event, 0);
        return S_OK;
    }
};
void StopAudio() {
    desiredAudio = false; audioPlaying = false;
    if (media) { media->Pause(); media->Shutdown(); media.Reset(); }
    audioSource.clear();
}
void EnsureAudio(const std::wstring& path) {
    if (media && audioSource == path) return;
    StopAudio(); audioError.clear();
    if (!fs::exists(path)) { audioError = L"音乐文件不存在，Q 弹继续运行。"; return; }
    if (!mfStarted) { audioError = L"Windows Media Foundation 不可用，Q 弹继续运行。"; return; }
    ComPtr<IMFAttributes> attributes; ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFMediaEngineNotify> notify; notify.Attach(new Notify());
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes.Get(), &media);
    if (SUCCEEDED(result)) {
        wchar_t url[32768]{}; DWORD length = 32768;
        result = UrlCreateFromPathW(path.c_str(), url, &length, 0);
        if (SUCCEEDED(result)) { auto source = SysAllocString(url); result = media->SetSource(source); SysFreeString(source); }
    }
    if (FAILED(result)) { StopAudio(); audioError = L"无法初始化本地音乐，Q 弹继续运行。"; return; }
    audioSource = path; desiredAudio = true; media->SetLoop(TRUE); media->SetAutoPlay(TRUE);
}
void UpdateAudio(bool reset = false) {
    bool wanted = shared && qb::Read(shared->stage) == qb::Ready && qb::Read(shared->enabled) && qb::Read(shared->scene) && Checked(Bgm) && !stopping;
    if (!wanted) { if (media) StopAudio(); return; }
    auto selected = Value(Path); auto path = selected.empty() ? defaultMusic.wstring() : selected;
    EnsureAudio(path); desiredAudio = true;
    if (media) {
        media->SetVolume(std::clamp(Slider(Volume) / 1000.0, 0.0, 1.0)); media->SetMuted(Checked(Muted));
        if (reset) media->SetCurrentTime(0);
        if (!audioPlaying || reset) { if (SUCCEEDED(media->Play())) audioPlaying = true; }
    }
}
void Disconnect() {
    StopAudio(); if (!shared) return;
    qb::Set(shared->enabled, 0); qb::Set(shared->shutdown, 1); stopping = true;
    Text(Status, L"正在等待游戏主线程恢复模型并移除 Hook……");
}
void Timer() {
    if (!shared) return;
    InterlockedExchange64(&shared->heartbeat, static_cast<LONG64>(GetTickCount64()));
    bool gone = WaitForSingleObject(gameProcess, 0) == WAIT_OBJECT_0;
    auto stage = qb::Read(shared->stage);
    if (gone || (stopping && moduleThread && WaitForSingleObject(moduleThread, 0) == WAIT_OBJECT_0)) {
        ReleaseConnection(); Text(Status, gone ? L"游戏已退出。" : L"已停止并恢复模型。");
        if (closing) DestroyWindow(window); return;
    }
    if (stopping) return;
    Check(Enable, qb::Read(shared->enabled) != 0);
    SetSlider(Amp, qb::Read(shared->amplitude)); Labels();
    if (stage == qb::Failed) {
        qb::Set(shared->enabled, 0); Check(Enable, false); EnableWindow(Item(Enable), FALSE);
        qbui::SetBadge(3); ShowError(shared->error); UpdateAudio(); Text(Status, L"关键校验失败，效果已停止。请断开连接。"); return;
    }
    qbui::SetBadge(qb::Read(shared->enabled) && qb::Read(shared->scene) ? 2 : 1);
    std::wstring status = qb::Read(shared->enabled) ? (qb::Read(shared->scene) ? L"运行中" : L"场景暂停（切图 / 过场 / GPose / 未登录）") : L"已连接，效果关闭";
    status += L"　已处理 " + std::to_wstring(qb::Read(shared->processed)) + L" / 跟踪 " +
        std::to_wstring(qb::Read(shared->tracked)) + L"　入口 " + std::to_wstring(qb::Read(shared->hooks));
    Text(Status, status);
    auto reset = qb::Read(shared->resetEcho); UpdateAudio(reset != audioReset); audioReset = reset;
    if (!audioError.empty()) ShowError(audioError);
}
HWND Add(const wchar_t* type, const wchar_t* label, DWORD style, int x, int y, int width, int height, int id = 0) {
    auto control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, qbui::Dp(x), qbui::Dp(y), qbui::Dp(width), qbui::Dp(height), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    qbui::Track(control, x, y, width, height); qbui::Style(control, type); return control;
}
void CreateControls() {
    font = qbui::body;
    Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 44, 139, 438, 220, Processes);
    SendMessageW(Item(Processes), CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), qbui::Dp(30));
    Add(L"BUTTON", L"刷新", BS_PUSHBUTTON | WS_TABSTOP, 494, 139, 84, 34, Refresh);
    Add(L"BUTTON", L"连接游戏", BS_PUSHBUTTON | WS_TABSTOP, 590, 139, 166, 34, Connect);
    Add(L"BUTTON", L"Q 弹总开关", BS_AUTOCHECKBOX | WS_TABSTOP, 42, 224, 330, 44, Enable); EnableWindow(Item(Enable), FALSE);
    Add(L"STATIC", L"强度 0.40", 0, 44, 282, 316, 24, AmpText);
    Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 40, 312, 332, 28, Amp);
    SendMessageW(Item(Amp), TBM_SETRANGE, TRUE, MAKELPARAM(0, 600));
    Add(L"STATIC", L"周期 0.45 秒", 0, 44, 350, 316, 24, PeriodText);
    Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 40, 378, 332, 28, Period);
    SendMessageW(Item(Period), TBM_SETRANGE, TRUE, MAKELPARAM(200, 1500));
    Add(L"BUTTON", L"随机相位", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 429, 326, 34, Random);
    Add(L"BUTTON", L"自己", BS_AUTOCHECKBOX | WS_TABSTOP, 424, 291, 332, 34, ScopeSelf);
    Add(L"BUTTON", L"其他玩家", BS_AUTOCHECKBOX | WS_TABSTOP, 424, 334, 332, 34, ScopePlayers);
    Add(L"BUTTON", L"NPC", BS_AUTOCHECKBOX | WS_TABSTOP, 424, 377, 332, 34, ScopeNpcs);
    Add(L"BUTTON", L"怪物 / BOSS", BS_AUTOCHECKBOX | WS_TABSTOP, 424, 420, 332, 34, ScopeMonsters);
    Add(L"BUTTON", L"BGM", BS_AUTOCHECKBOX | WS_TABSTOP, 480, 508, 128, 34, Bgm);
    Add(L"BUTTON", L"静音", BS_AUTOCHECKBOX | WS_TABSTOP, 620, 508, 138, 34, Muted);
    Add(L"STATIC", L"音量 35%", 0, 44, 552, 120, 24, VolumeText);
    Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 176, 549, 580, 28, Volume);
    SendMessageW(Item(Volume), TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 58, 600, 568, 24, Path);
    Add(L"BUTTON", L"选择音乐", BS_PUSHBUTTON | WS_TABSTOP, 652, 591, 104, 40, Browse);
    Add(L"STATIC", L"留空使用内置音乐 · 支持 MP3 / WAV", 0, 44, 645, 340, 22);
    Add(L"BUTTON", L"音乐出处", BS_PUSHBUTTON | WS_TABSTOP, 400, 643, 132, 28, Credits);
    Add(L"BUTTON", L"应用", BS_PUSHBUTTON | WS_TABSTOP, 548, 643, 96, 28, ApplyMusic);
    Add(L"BUTTON", L"重置节奏", BS_PUSHBUTTON | WS_TABSTOP, 654, 643, 102, 28, Reset);
    Add(L"BUTTON", L"启用快捷键", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 712, 250, 34, Hotkeys);
    Add(L"STATIC", L"未连接", 0, 44, 815, 712, 24, Status);
    Add(L"STATIC", L"版本不同会警告；关键函数或结构校验失败时停止。", 0, 44, 846, 712, 24, Version);
    Add(L"STATIC", L"", 0, 44, 875, 712, 24, Error);
    auto read = [](const wchar_t* key, int value) { return GetPrivateProfileIntW(L"QBounce", key, value, iniPath.c_str()); };
    SetSlider(Amp, std::clamp(read(L"Amplitude", 400), 0u, 600u)); SetSlider(Period, std::clamp(read(L"Period", 450), 200u, 1500u));
    SetSlider(Volume, std::clamp(read(L"Volume", 350), 0u, 1000u));
    for (auto pair : {std::pair{Random,L"Random"}, {ScopeSelf,L"Self"}, {ScopePlayers,L"Players"}, {ScopeNpcs,L"Npcs"}, {ScopeMonsters,L"Monsters"}, {Bgm,L"Bgm"}, {Muted,L"Muted"}, {Hotkeys,L"Hotkeys"}}) {
        bool defaultOn = pair.first >= ScopeSelf && pair.first <= Bgm;
        Check(pair.first, read(pair.second, defaultOn) != 0);
    }
    wchar_t path[32768]{}; GetPrivateProfileStringW(L"QBounce", L"AudioPath", L"", path, 32768, iniPath.c_str()); Text(Path, path);
    Labels(); RefreshProcesses(); qbui::Layout(); SetTimer(window, 1, 100, nullptr);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: window = hwnd; qbui::Init(hwnd); CreateControls(); return 0;
    case WM_PAINT: qbui::PaintCanvas(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: qbui::Layout(); return 0;
    case WM_MOUSEWHEEL: qbui::Scroll(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * qbui::Dp(48)); return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(window, SB_VERT, &info);
        int next = info.nPos;
        if (LOWORD(wparam) == SB_LINEUP) next -= qbui::Dp(32);
        else if (LOWORD(wparam) == SB_LINEDOWN) next += qbui::Dp(32);
        else if (LOWORD(wparam) == SB_PAGEUP) next -= static_cast<int>(info.nPage);
        else if (LOWORD(wparam) == SB_PAGEDOWN) next += static_cast<int>(info.nPage);
        else if (LOWORD(wparam) == SB_THUMBTRACK) next = info.nTrackPos;
        qbui::Scroll(next - info.nPos); return 0;
    }
    case WM_DPICHANGED: {
        qbui::dpi = HIWORD(wparam); qbui::Fonts(); font = qbui::body;
        auto suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        qbui::Layout(); return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {qbui::Dp(820), qbui::Dp(500)}; return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam); auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        SetBkMode(dc, TRANSPARENT); SetBkColor(dc, qbui::Card);
        SetTextColor(dc, id == Error ? qbui::Error : id == Version || id == 0 ? qbui::Muted : qbui::Text);
        return reinterpret_cast<LRESULT>(qbui::cardBrush);
    }
    case WM_MEASUREITEM: {
        auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam); if (item->CtlID == Processes) { item->itemHeight = qbui::Dp(28); return TRUE; } break;
    }
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID == Processes) {
            auto brush = CreateSolidBrush((item->itemState & ODS_SELECTED) ? qbui::SoftGold : qbui::Card); FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            int selected = item->itemID == static_cast<UINT>(-1) ? static_cast<int>(SendMessageW(Item(Processes), CB_GETCURSEL, 0, 0)) : static_cast<int>(item->itemID);
            if (selected >= 0) {
                int length = static_cast<int>(SendMessageW(Item(Processes), CB_GETLBTEXTLEN, selected, 0));
                if (length >= 0) { std::wstring text(length + 1, L'\0'); SendMessageW(Item(Processes), CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
                    auto rect = item->rcItem; rect.left += qbui::Dp(10); rect.right -= qbui::Dp(8); qbui::Label(item->hDC, text.c_str(), rect, font, qbui::Text); }
            }
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL: Labels(); PublishSettings(); SaveSettings(); return 0;
    case WM_TIMER: Timer(); return 0;
    case AudioEvent:
        if (wparam == MF_MEDIA_ENGINE_EVENT_CANPLAY && media && desiredAudio) { media->Play(); audioPlaying = true; }
        else if (wparam == MF_MEDIA_ENGINE_EVENT_ERROR) { audioError = L"音乐解码或输出失败，Q 弹继续运行。"; StopAudio(); ShowError(audioError); }
        return 0;
    case WM_COMMAND: {
        auto id = LOWORD(wparam);
        if (id == Refresh) RefreshProcesses();
        else if (id == Connect) {
            if (shared) Disconnect();
            else try { Text(Status, L"正在校验并连接……"); UpdateWindow(window); ConnectGame(); }
            catch (const std::wstring& error) { if (shared) qb::Set(shared->shutdown, 1); ReleaseConnection(); ShowError(error); Text(Status, L"未连接"); }
            catch (const std::exception&) { ReleaseConnection(); ShowError(L"连接失败，请查看游戏和文件状态。"); }
        } else if (id == Browse) {
            wchar_t path[32768]{}; OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = window;
            dialog.lpstrFilter = L"音频 MP3 / WAV\0*.mp3;*.wav\0所有文件\0*.*\0"; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&dialog)) { Text(Path, path); StopAudio(); PublishSettings(); SaveSettings(); }
        } else if (id == Credits) { ShellExecuteW(window, L"open", L"https://www.bilibili.com/video/BV1hXbN6VEKV/", nullptr, nullptr, SW_SHOWNORMAL); }
        else if (id == ApplyMusic) { StopAudio(); SaveSettings(); UpdateAudio(true); }
        else if (id == Reset) { if (shared) InterlockedIncrement(&shared->reset); }
        else if (id >= Enable && id <= Hotkeys && id != Path) { PublishSettings(); SaveSettings(); UpdateAudio(); }
        return 0;
    }
    case WM_CLOSE:
        SaveSettings(); closing = true;
        if (shared && gameProcess && WaitForSingleObject(gameProcess, 0) == WAIT_TIMEOUT) Disconnect();
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1); StopAudio(); ReleaseConnection(); qbui::Dispose(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int show) {
    instance = app; SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR graphicsToken = 0; Gdiplus::GdiplusStartupInput graphicsInput;
    Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr);
    mfStarted = SUCCEEDED(MFStartup(MF_VERSION));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    try {
        PWSTR local = nullptr; if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) throw std::wstring(L"无法获取本地配置目录。");
        dataDirectory = fs::path(local) / L"QBounceStandalone"; CoTaskMemFree(local); fs::create_directories(dataDirectory);
        iniPath = dataDirectory / L"settings.ini"; modulePath = Extract(101, L"q弹狒狒模块", L".dll");
        defaultMusic = Extract(102, L"bgm", L".mp3");
        WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = Procedure; cls.lpszClassName = L"QBounceStandaloneWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
        qbui::dpi = GetDpiForSystem(); RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        RECT bounds{0, 0, qbui::Dp(qbui::Width + 18), std::min<int>(qbui::Dp(qbui::Height), work.bottom - work.top - qbui::Dp(64))};
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, qbui::dpi);
        int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        window = CreateWindowExW(0, cls.lpszClassName, L"q弹狒狒 独立版 0.2.2 · someboy", style,
            work.left + (work.right - work.left - width) / 2, work.top + std::max(0L, (work.bottom - work.top - height) / 2), width, height, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::wstring(L"无法创建窗口。");
        ShowWindow(window, show); UpdateWindow(window);
        MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    } catch (const std::wstring& error) { MessageBoxW(nullptr, error.c_str(), L"q弹狒狒", MB_OK | MB_ICONERROR); }
    catch (const std::exception&) { MessageBoxW(nullptr, L"本地文件准备失败，请检查可用空间和写入权限。", L"q弹狒狒", MB_OK | MB_ICONERROR); }
    if (mfStarted) MFShutdown(); CoUninitialize(); if (graphicsToken) Gdiplus::GdiplusShutdown(graphicsToken); return 0;
}
