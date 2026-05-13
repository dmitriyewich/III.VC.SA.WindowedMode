#include "WindowedMode.h"
#include "AntiAfk.h"
#include <cstring>

namespace {

bool FileExistsInExeDirectory(const char* relativeName)
{
	char exePath[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
		return false;
	char* slash = std::strrchr(exePath, '\\');
	if (!slash)
		return false;
	*slash = '\0';

	char path[MAX_PATH] = {};
	if (sprintf_s(path, "%s\\%s", exePath, relativeName) == -1)
		return false;

	const DWORD attrs = GetFileAttributesA(path);
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsModloaderPresent()
{
	if (GetModuleHandleA("modloader.asi") != nullptr || GetModuleHandleA("modloader") != nullptr)
		return true;
	return FileExistsInExeDirectory("modloader.asi");
}

} // namespace

void WindowedMode::InitGtaSA()
{
	inst = new WindowedMode(
		GameTitle::GTA_SA,
		0xC8D4C0, // gameState
		0xC17040, // rsGlobal
		0xC97C28, // d3dDevice
		0xC9C040, // d3dPresentParams
		0xC97C48, // rwVideoModes
		0x7F2CC0, // RwEngineGetNumVideoModes
		0x7F2D20, // RwEngineGetCurrentVideoMode
		0xBA6748  // FrontEndMenuManager
	);

	strcpy_s(inst->windowClassName, "Grand theft auto San Andreas");
	inst->windowIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(100));

	// Debug: check if somebody already modified memory we want to hook
#ifdef DEBUG
	VerifyMemory("Device selection", 0x746225, 1, 0x65BFB3B6);
	VerifyMemory("RsMouseSetPos call", 0x53E9F1, 5, 0x9D112499);
	VerifyMemory("CreateWindow", 0x7455D5, 6, 0x4A88D8AA);
	VerifyMemory("InitPresentationParams", 0x7F670A, 6, 0xAB349BBF);
	VerifyMemory("InitD3dDevice", 0x7F6800, 6, 0xEDAE7102);
#endif

	// SA 1.0 US: allow a second gta_sa.exe — patch call CheckForOtherSA (same 5 bytes as mpgtasa:
	// 3x NOP + xor eax,eax so following test eax sees "no duplicate". Plain 5x NOP leaves eax undefined.)
	// With Mod Loader this patch is skipped — multiprocess + modloader are not supported together.
	// AmyrAhmady/multi-process-gtasa also patches 0x406946 and 0x53BC78; without them a second instance
	// can stall after the first frames even when 74872D is patched.
	if (!IsModloaderPresent())
	{
		static const uint8_t kMultiInstancePatch5[] = { 0x90, 0x90, 0x90, 0x31, 0xC0 };
		injector::WriteMemoryRaw(0x74872D, (void*)kMultiInstancePatch5, sizeof(kMultiInstancePatch5), true);
		injector::WriteMemory<uint8_t>(0x406946, 0, true);
		injector::WriteMemory<uint8_t>(0x53BC78, 0, true);
	}

	// do not show device selection dialog in case of multiple display monitors
	injector::WriteMemory(0x746225, BYTE(0xEB), true);

	// disable recentering cursor pos. Frees mouse when window inactive but game not paused
	injector::MakeNOP(0x53E9F1, 5); // call to RsMouseSetPos

	// patch call to CreateWindowExA
	injector::MakeNOP(0x7455D5, 6);
	injector::MakeCALL(0x7455D5, WindowedMode::InitWindow);

	struct Patch_InitPresentationParams // just before D3D device is created
	{
		void operator()(injector::reg_pack& regs)
		{
			regs.ecx = *(DWORD*)(0xC97C4C); // original action replaced by the patch

			inst->WindowCalculateGeometry();
		}
	}; injector::MakeInline<Patch_InitPresentationParams>(0x7F670A, 0x7F6710);

	struct Path_InitD3dDevice // just after D3D device has been created
	{
		void operator()(injector::reg_pack& regs)
		{
			*(DWORD*)(0xC9808C) = regs.ebp; // original action replaced by the patch

			inst->InitD3dDevice();
		}
	}; injector::MakeInline<Path_InitD3dDevice>(0x7F6800, 0x7F6806);

	AntiAfk::InstallGameHooks();
}
