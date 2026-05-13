#include "AntiAfk.h"
#include "WindowedMode.h"

#include <MinHook.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <Windows.h>

namespace
{

struct RwV2d
{
	float x;
	float y;
};

using RsMouseSetPos_t = int(__cdecl*)(RwV2d*);
using CPadUpdatePads_t = void(__cdecl*)();

constexpr uintptr_t kRsMouseSetPos = 0x6194A0;
constexpr uintptr_t kCPadUpdatePads = 0x541DD0;

struct Settings
{
	bool afkMode = false;
	bool dontSetCursor = false;
	bool dontForegroundWindow = false;
	int intervalMs = 2800;
};

Settings g;
DWORD g_lastPulseTick = 0;

RsMouseSetPos_t g_origRsMouseSetPos = nullptr;
CPadUpdatePads_t g_origCPadUpdatePads = nullptr;

bool g_hooksInstalled = false;

bool GetModulePathFromAddress(char* out, size_t cap, const void* addrInThisModule)
{
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(
	        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        static_cast<LPCSTR>(addrInThisModule),
	        &mod))
		return false;
	return GetModuleFileNameA(mod, out, static_cast<DWORD>(cap)) != 0;
}

bool ParseJsonBool(const std::string& text, const char* key, bool defaultValue)
{
	const std::string quoted = std::string("\"") + key + "\"";
	const size_t keyPos = text.find(quoted);
	if (keyPos == std::string::npos)
		return defaultValue;

	size_t colon = text.find(':', keyPos + quoted.size());
	if (colon == std::string::npos)
		return defaultValue;

	size_t i = colon + 1;
	while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
		++i;
	if (i >= text.size())
		return defaultValue;

	if (text.compare(i, 4, "true") == 0)
		return true;
	if (text.compare(i, 5, "false") == 0)
		return false;
	if (text[i] == '1' && (i + 1 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 1]))))
		return true;
	if (text[i] == '0' && (i + 1 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 1]))))
		return false;
	return defaultValue;
}

int ParseJsonInt(const std::string& text, const char* key, int defaultValue)
{
	const std::string quoted = std::string("\"") + key + "\"";
	const size_t keyPos = text.find(quoted);
	if (keyPos == std::string::npos)
		return defaultValue;

	size_t colon = text.find(':', keyPos + quoted.size());
	if (colon == std::string::npos)
		return defaultValue;

	size_t i = colon + 1;
	while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
		++i;

	int value = 0;
	if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i])))
		return defaultValue;

	while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
	{
		value = value * 10 + (text[i] - '0');
		++i;
	}
	return (std::max)(250, (std::min)(value, 600000));
}

bool ReadWholeFile(const char* path, std::string& out)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
		return false;
	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return false;
	}
	const long len = ftell(f);
	if (len <= 0 || len > 4 * 1024 * 1024)
	{
		fclose(f);
		return false;
	}
	rewind(f);
	out.resize(static_cast<size_t>(len));
	if (len != 0 && fread(out.data(), 1, static_cast<size_t>(len), f) != static_cast<size_t>(len))
	{
		fclose(f);
		out.clear();
		return false;
	}
	fclose(f);
	return true;
}

int __cdecl DetourRsMouseSetPos(RwV2d* pos)
{
	if (g.dontSetCursor)
		return 0;
	if (g_origRsMouseSetPos == nullptr)
		return 0;
	return g_origRsMouseSetPos(pos);
}

void PulseIdleInput(HWND window)
{
	if (window == nullptr)
		return;

	if (!g.dontSetCursor)
	{
		INPUT in[2] = {};
		in[0].type = INPUT_MOUSE;
		in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
		in[0].mi.dx = 1;
		in[0].mi.dy = 0;
		in[1].type = INPUT_MOUSE;
		in[1].mi.dwFlags = MOUSEEVENTF_MOVE;
		in[1].mi.dx = -1;
		in[1].mi.dy = 0;
		SendInput(2, in, sizeof(INPUT));
	}
	else
	{
		PostMessageA(window, WM_NULL, 0, 0);
	}
}

void __cdecl DetourCPadUpdatePads()
{
	if (g_origCPadUpdatePads != nullptr)
		g_origCPadUpdatePads();

	if (!g.afkMode || inst == nullptr || inst->gameTitle != WindowedMode::GTA_SA)
		return;

	HWND wnd = inst->window;
	if (wnd == nullptr)
		return;

	const DWORD now = GetTickCount();
	if (g_lastPulseTick != 0)
	{
		const DWORD dt = now - g_lastPulseTick;
		if (dt < static_cast<DWORD>(g.intervalMs))
			return;
	}
	g_lastPulseTick = now;

	PulseIdleInput(wnd);
}

} // namespace

namespace AntiAfk
{

void LoadFromIni(CIniReader& ini)
{
	g.afkMode = ini.ReadInteger("antiafk", "AFKMode", 0) != false;
	g.dontSetCursor = ini.ReadInteger("antiafk", "DontSetCursor", 0) != false;
	g.dontForegroundWindow = ini.ReadInteger("antiafk", "DontForegroundWindow", 0) != false;
	g.intervalMs = ini.ReadInteger("antiafk", "intervalMs", 2800);
	g.intervalMs = std::clamp(g.intervalMs, 250, 600000);
	(void)g.dontForegroundWindow;
}

void TryLoadJsonBesideModule()
{
	char modulePath[MAX_PATH] = {};
	if (!GetModulePathFromAddress(modulePath, MAX_PATH, reinterpret_cast<const void*>(&AntiAfk::InstallGameHooks)))
		return;

	char jsonPath[MAX_PATH] = {};
	if (strcpy_s(jsonPath, modulePath) != 0)
		return;
	char* dot = std::strrchr(jsonPath, '.');
	if (dot == nullptr)
		return;
	if (strcpy_s(dot, sizeof(jsonPath) - static_cast<size_t>(dot - jsonPath), ".json") != 0)
		return;

	std::string text;
	if (!ReadWholeFile(jsonPath, text))
		return;

	g.afkMode = ParseJsonBool(text, "AFKMode", g.afkMode);
	g.dontSetCursor = ParseJsonBool(text, "DontSetCursor", g.dontSetCursor);
	g.dontForegroundWindow = ParseJsonBool(text, "DontForegroundWindow", g.dontForegroundWindow);
	g.intervalMs = ParseJsonInt(text, "intervalMs", g.intervalMs);
}

void SaveToIni(CIniReader& ini)
{
	ini.WriteString("antiafk", "AFKMode",
	                StringPrintf("%d\t\t; same as #AntiAFK JSON: periodic idle keep-alive", g.afkMode ? 1 : 0));
	ini.WriteString("antiafk", "DontSetCursor",
	                StringPrintf("%d\t; 1: RsMouseSetPos returns without moving the system cursor", g.dontSetCursor ? 1 : 0));
	ini.WriteString("antiafk", "DontForegroundWindow",
	                StringPrintf("%d\t; reserved (AIR reads it); no SetForegroundWindow hook here", g.dontForegroundWindow ? 1 : 0));
	ini.WriteString("antiafk", "intervalMs",
	                StringPrintf("%d\t; AFK pulse period when AFKMode=1 (250..600000), runs after CPad::UpdatePads", g.intervalMs));
}

void InstallGameHooks()
{
	if (g_hooksInstalled)
		return;
	g_hooksInstalled = true;

	MH_STATUS st = MH_Initialize();
	if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		return;

	void* origMouse = nullptr;
	void* origPads = nullptr;
	if (MH_CreateHook(reinterpret_cast<LPVOID>(kRsMouseSetPos), reinterpret_cast<LPVOID>(&DetourRsMouseSetPos), &origMouse) != MH_OK)
		return;
	if (MH_CreateHook(reinterpret_cast<LPVOID>(kCPadUpdatePads), reinterpret_cast<LPVOID>(&DetourCPadUpdatePads), &origPads) != MH_OK)
	{
		MH_RemoveHook(reinterpret_cast<LPVOID>(kRsMouseSetPos));
		return;
	}

	g_origRsMouseSetPos = reinterpret_cast<RsMouseSetPos_t>(origMouse);
	g_origCPadUpdatePads = reinterpret_cast<CPadUpdatePads_t>(origPads);

	if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
	{
		MH_RemoveHook(reinterpret_cast<LPVOID>(kRsMouseSetPos));
		MH_RemoveHook(reinterpret_cast<LPVOID>(kCPadUpdatePads));
		g_origRsMouseSetPos = nullptr;
		g_origCPadUpdatePads = nullptr;
	}
}

} // namespace AntiAfk
