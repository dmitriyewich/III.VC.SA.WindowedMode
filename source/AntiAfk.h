#pragma once

#include "misc.h"

class WindowedMode;

namespace AntiAfk
{
	void LoadFromIni(CIniReader& ini);
	void TryLoadJsonBesideModule();
	void SaveToIni(CIniReader& ini);
	/// GTA SA 1.0 US: MinHook on RsMouseSetPos (0x6194A0) and CPad::UpdatePads (0x541DD0).
	void InstallGameHooks();
}
