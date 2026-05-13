#include "WindowedMode.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		char name[128];
		injector::address_manager::singleton().GetVersionText(name);

		if (!_stricmp(name, "GTA III 1.0.0.0 UNK_REGION"))
		{
			WindowedMode::InitGta3();
		}
		else if (!_stricmp(name, "GTA VC 1.0.0.0 UNK_REGION"))
		{
			WindowedMode::InitGtaVC();
		}
		else if (!_stricmp(name, "GTA SA 1.0.0.0 US"))
		{
			WindowedMode::InitGtaSA();
		}
		else
		{
			ShowError("Game version \"%s\" is not supported! \nSee readme file.", name);
		}
	}

	return TRUE;
}
