#include "WindowedMode.h"
#include "Windowed_Gta3.h"
#include "Windowed_GtaVC.h"
#include "Windowed_GtaSA.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib") // DwmGetWindowAttribute
#pragma comment(lib, "winmm.lib") // timeGetTime

// list of popular display aspect ratios
const WindowedMode::AspectRatioInfo WindowedMode::AspectRatios[] = {
	{ "1:1", 1.0f / 1.0f },
	{ "2:1", 2.0f / 1.0f },
	{ "4:3", 4.0f / 3.0f },
	{ "5:4", 5.0f / 4.0f },
	{ "10:7", 10.0f / 7.0f }, // GTA default aspect
	{ "16:9", 16.0f / 9.0f },
	{ "16:10", 16.0f / 10.0f }
};

WindowedMode::WindowedMode(
	GameTitle gameTitle,
	uintptr_t gameState,
	uintptr_t rsGlobal,
	uintptr_t d3dDevice,
	uintptr_t d3dPresentParams,
	uintptr_t rwVideoModes,
	uintptr_t RwEngineGetNumVideoModes,
	uintptr_t RwEngineGetCurrentVideoMode,
	uintptr_t frontEndMenuManager
) :
	gameTitle(gameTitle),
	gameState(*(GameState*)gameState),
	rsGlobal((RsGlobalType*)rsGlobal),
	d3dDevice(*(IDirect3DDevice8**)d3dDevice),
	d3dPresentParams8((D3DPRESENT_PARAMETERS*)d3dPresentParams),
	rwVideoModes((DisplayMode**)rwVideoModes),
	RwEngineGetNumVideoModes(*(DWORD(*)())RwEngineGetNumVideoModes),
	RwEngineGetCurrentVideoMode(*(DWORD(*)())RwEngineGetCurrentVideoMode),
	frontEndMenuManager(frontEndMenuManager)
{}

HWND __stdcall WindowedMode::InitWindow(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
	WNDCLASSA oriClass;
	if (!GetClassInfoA(hInstance, inst->windowClassName, &oriClass))
	{
		ShowError("Game window class not found!");
		return NULL;
	}
	inst->oriWindowProc = oriClass.lpfnWndProc;

	inst->InitConfig();
	bool maximize = inst->LoadConfig();
	bool center = (inst->windowPos.x == -1) || (inst->windowPos.y == -1);

	inst->WindowCalculateGeometry(center);
	inst->WindowUpdateTitle();

	// Per-process unique class name so a second game instance never shares the same
	// registered class atom / name as the first (UnregisterClass+reuse is fragile when
	// multiple gta_sa.exe run without modloader).
	char registeredClassName[96];
	if (sprintf_s(registeredClassName, "%s_%08lX", inst->windowClassName,
	              static_cast<unsigned long>(GetCurrentProcessId())) < 0)
	{
		ShowError("WindowedMode: window class name buffer overflow.");
		return NULL;
	}

	WNDCLASSA wndClass = {};
	wndClass.hInstance = hInstance;
	wndClass.lpszClassName = registeredClassName;
	wndClass.style = 0;
	wndClass.hIcon = inst->windowIcon;
	wndClass.hCursor = LoadCursor(hInstance, IDC_ARROW);
	wndClass.lpszMenuName = NULL;
	wndClass.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
	wndClass.lpfnWndProc = &WindowedMode::WindowProc;
	wndClass.cbClsExtra = 0;
	wndClass.cbWndExtra = 0;

	if (!RegisterClassA(&wndClass))
	{
		const DWORD err = GetLastError();
		if (err != ERROR_CLASS_ALREADY_EXISTS)
		{
			ShowError("WindowedMode: RegisterClass failed (error %lu).", static_cast<unsigned long>(err));
			return NULL;
		}
		UnregisterClassA(registeredClassName, hInstance);
		if (!RegisterClassA(&wndClass))
		{
			ShowError("WindowedMode: RegisterClass failed after retry (error %lu).",
			          static_cast<unsigned long>(GetLastError()));
			return NULL;
		}
	}

	inst->window = CreateWindowEx(
		inst->WindowStyleEx(),
		registeredClassName,
		inst->windowTitle,
		inst->WindowStyle(),
		inst->windowPos.x, inst->windowPos.y,
		inst->windowSize.x, inst->windowSize.y,
		NULL, // parent
		NULL, // menu
		hInstance,
		0);

	// SA 1.0 US: HWND mirror for D3D (GTAMods wiki 0xC97C1C). Do not write undocumented slots.
	if (inst->gameTitle == GameTitle::GTA_SA && inst->window)
	{
		injector::WriteMemory<void*>(0xC97C1C, inst->window, true);
	}

	if (maximize)
		PostMessage(inst->window, WM_SYSCOMMAND, SC_MAXIMIZE, 0); // maximize and perofm updates
	else if (inst->windowMode == WindowMode::Windowed)
		inst->WindowCalculateGeometry(center, true); // now modern styles border padding can be calculcated

	UpdateWindow(inst->window);
	inst->MouseUpdate(true); // lock cursor in the window until main menu appears

	return inst->window;
}

void WindowedMode::InitD3dDevice()
{
	if (d3dDevice == nullptr)
	{
		return;
	}

	auto vTable = *(uintptr_t**)d3dDevice;
	DWORD oldProtect;
	injector::UnprotectMemory(vTable, 20 * sizeof(uintptr_t), oldProtect);

	if (IsD3D9())
	{
		d3dResetOri = reinterpret_cast<decltype(d3dResetOri)>(vTable[16]);
		vTable[16] = (uintptr_t)&D3dResetHook;

		d3dPresentOri = reinterpret_cast<decltype(d3dPresentOri)>(vTable[17]);
		vTable[17] = (uintptr_t)&D3dPresentHook;
	}
	else
	{
		d3dResetOri = reinterpret_cast<decltype(d3dResetOri)>(vTable[14]);
		vTable[14] = (uintptr_t)&D3dResetHook;

		d3dPresentOri = reinterpret_cast<decltype(d3dPresentOri)>(vTable[15]);
		vTable[15] = (uintptr_t)&D3dPresentHook;
	}
}

void WindowedMode::InitConfig()
{
	auto attr = GetFileAttributes(config.GetIniPath().string().c_str());
	
	if (attr == INVALID_FILE_ATTRIBUTES) // does not exists
		SaveConfig();
}

bool WindowedMode::LoadConfig()
{
	windowMode = (WindowMode)config.ReadInteger("window", "mode", WindowMode::Windowed);
	windowMode = std::clamp(windowMode, WindowMode::Min, WindowMode::Max);

	bool maximize = config.ReadInteger("window", "maximized", 0) != false;

	windowPosWindowed.x = config.ReadInteger("window", "positionX", -1);
	windowPosWindowed.y = config.ReadInteger("window", "positionY", -1);

	windowSizeWindowed.x = max(config.ReadInteger("window", "resolutionX", Resolution_Default.x), Resolution_Min.x);
	windowSizeWindowed.y = max(config.ReadInteger("window", "resolutionY", Resolution_Default.y), Resolution_Min.y);
	
	windowPos = windowPosWindowed;
	windowSize = windowSizeClient = windowSizeWindowed;
	
	menuFrameRateLimit = config.ReadInteger("game", "menuFPS", 30);
	autoPause = config.ReadInteger("game", "autoPause", true) != false;
	autoResume = config.ReadInteger("game", "autoResume", true) != false;

	return maximize;
}

void WindowedMode::SaveConfig()
{
	config.WriteString("window", "mode",		StringPrintf("%d\t\t\t; 1: window, 2: window borderless, 3: fullscreen", windowMode));
	config.WriteString("window", "maximized",	StringPrintf("%d", IsZoomed(window)));
	config.WriteString("window", "positionX",	StringPrintf("%d\t; -1: centered", windowPosWindowed.x));
	config.WriteString("window", "positionY",	StringPrintf("%d\t; -1: centered", windowPosWindowed.y));
	config.WriteString("window", "resolutionX",	StringPrintf("%d", windowSizeWindowed.x));
	config.WriteString("window", "resolutionY",	StringPrintf("%d", windowSizeWindowed.y));
	
	config.WriteString("game", "menuFPS",		StringPrintf("%d\t\t; frame rate limit for main menu. 0: unlimited", menuFrameRateLimit));
	config.WriteString("game", "autoPause",		StringPrintf("%d\t\t; pause the game on window deactivation", autoPause));
	config.WriteString("game", "autoResume",	StringPrintf("%d\t; resume the game on window activation", autoResume));
}

int WindowedMode::FindAspectRatio(POINT resolution, float treshold)
{
	auto ratio = float(resolution.x) / resolution.y;

	// find best match in
	int idx = -1;
	float dist = 9999.0f;
	for (size_t i = 0; i < _countof(AspectRatios); i++)
	{
		auto diff = fabs(AspectRatios[i].ratio - ratio);
		if (diff < dist)
		{
			idx = i;
			dist = diff;
		}
	}

	return dist <= treshold ? idx : -1;
}

DWORD WindowedMode::WindowStyle() const
{
	return WS_VISIBLE | WS_CLIPSIBLINGS | ((windowMode == WindowMode::Windowed) ?
		WS_OVERLAPPEDWINDOW :
		WS_POPUP);
}

DWORD WindowedMode::WindowStyleEx() const
{
	return (windowMode == WindowMode::Windowed) ?
		0 : // WS_EX_CLIENTEDGE
		0;
}

void WindowedMode::WindowCalculateGeometry(bool center, bool resizeWindow)
{
	windowUpdating = true;

	POINT windowCenter = { windowPos.x + windowSize.x / 2, windowPos.y + windowSize.y / 2};
	auto monitorRect = GetMonitorRect(windowCenter);
	auto monitorWidth = monitorRect.right - monitorRect.left;
	auto monitorHeight = monitorRect.bottom - monitorRect.top;
	bool monitorSingle = GetSystemMetrics(SM_CMONITORS) <= 1;

	// size
	if (windowMode == WindowMode::Fullscreen)
	{
		windowPos.x = monitorRect.left;
		windowPos.y = monitorRect.top;
		windowSize.x = windowSizeClient.x = monitorWidth;
		windowSize.y = windowSizeClient.y = monitorHeight;
	}
	else if (!IsZoomed(window)) // not maximized windowed modes
	{
		windowSize = SizeFromClient(windowSizeWindowed);

		if (monitorSingle) // limit window size to desktop
		{
			windowSize.x = min(windowSize.x, monitorWidth);
			windowSize.y = min(windowSize.y, monitorHeight);
		}

		windowSizeClient = windowSizeWindowed = ClientFromSize(windowSize);

		// window position
		if (center)
		{
			windowPosWindowed.x = (monitorWidth - windowSize.x) / 2;
			windowPosWindowed.y = (monitorHeight - windowSize.y) / 2;
		}
		
		if (monitorSingle) // keep entire window on the screen
		{
			windowPosWindowed.x = max(windowPosWindowed.x, monitorRect.left);
			if (windowPosWindowed.x + windowSize.x > monitorRect.right)
			{
				windowPosWindowed.x = monitorRect.right - windowSize.x;
			}

			windowPosWindowed.y = max(windowPosWindowed.y, monitorRect.top);
			if (windowPosWindowed.y + windowSize.y > monitorRect.bottom)
			{
				windowPosWindowed.y = monitorRect.bottom - windowSize.y;
			}
		}

		windowPos = windowPosWindowed;
	}

	// apply to the window
	if (resizeWindow && !IsZoomed(window))
	{
		SetWindowLong(window, GWL_STYLE, WindowStyle());
		SetWindowLong(window, GWL_EXSTYLE, WindowStyleEx());
		SetWindowPos(window, 0, 0, 0, 0, 0, SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOSENDCHANGING | SWP_FRAMECHANGED | SWP_SHOWWINDOW); // update the frame
		auto padding = GetFrameSize(true);

		SetWindowPos(window, 0,
			windowPos.x - padding.left,
			windowPos.y - padding.top,
			windowSize.x,
			windowSize.y,
			SWP_NOOWNERZORDER | SWP_NOSENDCHANGING | SWP_SHOWWINDOW);

		WindowUpdateTitle();
	}

	// apply resolution to game internals
	if (gameTitle == GameTitle::GTA_SA)
	{
		rsGlobalSA->ps->fullScreen = false;
		rsGlobalSA->ps->window = window;
		rsGlobalSA->MaximumWidth = windowSizeClient.x;
		rsGlobalSA->MaximumHeight = windowSizeClient.y;
	}
	else
	{
		rsGlobal->ps->fullScreen = false;
		rsGlobal->ps->window = window;
		rsGlobal->screenWidth = rsGlobal->MaximumWidth = windowSizeClient.x;
		rsGlobal->screenHeight = rsGlobal->MaximumHeight = windowSizeClient.y;
	}

	if (IsD3D9())
	{
		d3dPresentParams9->Windowed = TRUE;
		d3dPresentParams9->hDeviceWindow = window;
		d3dPresentParams9->BackBufferWidth = windowSizeClient.x;
		d3dPresentParams9->BackBufferHeight = windowSizeClient.y;
		d3dPresentParams9->BackBufferFormat = D3DFMT_A8R8G8B8;
		d3dPresentParams9->SwapEffect = D3DSWAPEFFECT_DISCARD;
		d3dPresentParams9->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
		d3dPresentParams9->FullScreen_RefreshRateInHz = 0;
	}
	else
	{
		d3dPresentParams8->Windowed = TRUE;
		d3dPresentParams8->hDeviceWindow = window;
		d3dPresentParams8->BackBufferWidth = windowSizeClient.x;
		d3dPresentParams8->BackBufferHeight = windowSizeClient.y;
		d3dPresentParams8->BackBufferFormat = D3DFMT_X8R8G8B8;
		d3dPresentParams8->SwapEffect = D3DSWAPEFFECT_DISCARD;
		d3dPresentParams8->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
		d3dPresentParams8->FullScreen_RefreshRateInHz = 0;
	}

	// write the resolution into video display modes list
	if (*rwVideoModes)
	{
		// backup display modes infos before making any changes
		if (videoModesBackup.empty())
		{
			auto count = RwEngineGetNumVideoModes();
			videoModesBackup.resize(count);
			memcpy(videoModesBackup.data(), *rwVideoModes, count * sizeof(DisplayMode));
		}

		static int prevVideoMode = -1;
		int currVideoMode = RwEngineGetCurrentVideoMode();
	
		// restore previous display mode info to its original state
		if (prevVideoMode != -1 && currVideoMode != prevVideoMode) (*rwVideoModes)[prevVideoMode] = videoModesBackup[prevVideoMode];
		prevVideoMode = currVideoMode;

		// write modified resolution into current display mode info
		auto& mode = (*rwVideoModes)[currVideoMode];
		mode.width = windowSizeClient.x;
		mode.height = windowSizeClient.y;
		mode.format = IsD3D9() ? d3dPresentParams9->BackBufferFormat : d3dPresentParams8->BackBufferFormat;
		mode.refreshRate = IsD3D9() ? d3dPresentParams9->FullScreen_RefreshRateInHz : d3dPresentParams8->FullScreen_RefreshRateInHz;
		mode.flags &= ~1; // clear fullscreen flag
	}

	windowUpdating = false;
}

void WindowedMode::WindowResize(POINT resolution)
{
	if (windowMode == WindowMode::Fullscreen)
		windowMode = WindowMode::Windowed;

	windowSizeWindowed = resolution;
	windowPosWindowed = { -1, -1 }; // re-center on resolution change
	WindowCalculateGeometry(true, true); // and resize the window
	SaveConfig();
}

void WindowedMode::WindowModeCycle()
{
	if (IsIconic(window)) return; // minimized

	if (!HasFocus(window)) return; // window inactive

	if (IsZoomed(window)) // maximized
	{
		windowMode = WindowedMode::Min;
		ShowWindow(window, SW_RESTORE);
	}
	else
	{
		BYTE& mode = *(BYTE*)&windowMode;
		mode += 1;
		if (mode > WindowedMode::Max) mode = WindowedMode::Min;
	}

	WindowCalculateGeometry(false, true);
	SaveConfig();
}

void WindowedMode::WindowUpdateTitle()
{
	if (windowMode == WindowMode::Windowed && HasFocus(window))
	{
		std::string aspectTxt;
		auto idx = FindAspectRatio(windowSizeClient);
		if (idx != -1)
		{
			aspectTxt = StringPrintf(" (%s)", AspectRatios[idx].name);
		}

		sprintf_s(windowTitle, "%s | %ux%u%s @ %u fps",
			rsGlobal->AppName,
			windowSizeClient.x,
			windowSizeClient.y,
			aspectTxt.c_str(),
			fpsCounter.get());
	}
	else
		strcpy_s(windowTitle, rsGlobal->AppName);
	
	if (window)
		SetWindowText(window, windowTitle);
}

LRESULT APIENTRY WindowedMode::WindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		// window focus/defocus
		case WM_ACTIVATE:
		{
			auto result = (LOWORD(wParam) == WA_INACTIVE) ?
				DefWindowProc(wnd, msg, wParam, lParam) : // don't pause game on defocus
				CallWindowProc(inst->oriWindowProc, wnd, msg, wParam, lParam);

			// handle automatic pause/resume
			if (inst->gameState == Playing_Game)
			{
				switch(LOWORD(wParam))
				{
					case WA_INACTIVE:
						if (inst->autoPause && !inst->IsMainMenuVisible())
						{
							inst->SwitchMainMenu(true);
							inst->autoPauseExecuted = true;
						}
						break;

					case WA_CLICKACTIVE: // mouse click
						if (!IsCursorInClientRect(wnd))
						{
							inst->autoPauseExecuted = false;
							break; // user clicked on the window caption or edge
						}
						[[fallthrough]];

					case WA_ACTIVE:
						if (inst->autoResume && 
							(!inst->autoPause || inst->autoPauseExecuted) && 
							inst->IsMainMenuVisible()) // TODO: check if not in some submenu
						{
							inst->autoPauseExecuted = false;
							inst->SwitchMainMenu(false);
						}
						break;
				}
			}

			inst->WindowUpdateTitle();
			inst->MouseUpdate(true);
			return result;
		}

		// don't pause game on defocus
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
			return DefWindowProc(wnd, msg, wParam, lParam);
		
		// restore proper handling of ShowCursor
		case WM_SETCURSOR:
			return DefWindowProc(wnd, msg, wParam, lParam);

		// do not send keyboard events to the inactive window
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		{
			if (!HasFocus(wnd))
				return DefWindowProc(wnd, msg, wParam, lParam); // bypass the game
			
			// handle Alt+Enter key combination
			if (wParam == VK_RETURN && IsKeyDown(VK_MENU))
			{
				inst->WindowModeCycle();
				return DefWindowProc(wnd, msg, wParam, lParam); // bypass the game
			}

			break;
		}

		// handle the window menu Alt+Key hotkey messages
		case WM_SYSCOMMAND:
			if (wParam == SC_KEYMENU)
				return S_OK; // handled
			break;

		// do not send mouse events to the inactive window
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONDBLCLK:
		case WM_MOUSEACTIVATE:
		case WM_MOUSEHOVER:
		case WM_MOUSEMOVE:
		case WM_MOUSEWHEEL:
		{
			auto hasFocus = HasFocus(wnd);
			auto inClient = IsCursorInClientRect(wnd);

			if (!hasFocus && inClient)
			{
				SetCursor(LoadCursor(NULL,IDC_ARROW));
			}

			if (!hasFocus || !inClient)
			{
				return DefWindowProc(wnd, msg, wParam, lParam); // bypass the game
			}
			break;
		}

		case WM_STYLECHANGING:
		{
			auto styles = (STYLESTRUCT*)lParam;
			
			if (wParam == GWL_STYLE)
			{
				auto mask = WS_MINIMIZE | WS_MAXIMIZE;
				styles->styleNew &= mask;
				styles->styleNew |= inst->WindowStyle();
			}
			else if (wParam == GWL_EXSTYLE)
				styles->styleNew = inst->WindowStyleEx();
				
			break;
		}

		// user dragging the window edge
		case WM_SIZING:
		{
			auto wndRect = (RECT*)lParam;
			auto size = inst->ClientFromSize({
				wndRect->right - wndRect->left,
				wndRect->bottom - wndRect->top
			});

			// minimal game resolution
			size.x = max(size.x, Resolution_Min.x);
			size.y = max(size.y, Resolution_Min.y);

			// snap to known aspect ratios
			auto idx = FindAspectRatio(size, 0.02f);
			if (idx != -1)
			{
				auto currAspect = float(size.x) / size.y;

				switch(wParam)
				{
					case WMSZ_LEFT:
					case WMSZ_RIGHT:
						size.x = LONG(size.y * AspectRatios[idx].ratio);
						break;

					case WMSZ_TOP:
					case WMSZ_BOTTOM:
						size.y = LONG(size.x / AspectRatios[idx].ratio);
						break;

					default: // sizing both X and Y
					{
						if (currAspect < AspectRatios[idx].ratio)
							size.x = LONG(size.y * AspectRatios[idx].ratio);
						else
							size.y = LONG(size.x / AspectRatios[idx].ratio);
					}
				}
			}

			// update window title immediately
			inst->windowSizeClient.x = size.x;
			inst->windowSizeClient.y = size.y;
			inst->WindowUpdateTitle();

			// apply modified window size
			size = inst->SizeFromClient(size);
			if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT) wndRect->left = wndRect->right - size.x;
			if (wParam == WMSZ_RIGHT || wParam == WMSZ_TOPRIGHT || wParam == WMSZ_BOTTOMRIGHT) wndRect->right = wndRect->left + size.x;
			if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT) wndRect->top = wndRect->bottom - size.y;
			if (wParam == WMSZ_BOTTOM || wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_BOTTOMRIGHT) wndRect->bottom = wndRect->top + size.y;

			return DefWindowProc(wnd, msg, wParam, lParam);
		}

		case WM_EXITSIZEMOVE:
			inst->WindowCalculateGeometry(false, true);

		// minimize, maximize, restore
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED && wParam != SIZE_MAXHIDE) // prevent game from updating resolution for minimized window
				CallWindowProc(inst->oriWindowProc, wnd, msg, wParam, lParam); // inform the game
			return DefWindowProc(wnd, msg, wParam, lParam); // call default as otherwise maximization will not work correctly on later Windows versions

		// position or size changed
		case WM_WINDOWPOSCHANGED:
		{
			if (inst->windowUpdating || IsIconic(wnd)) break; // minimized

			bool updated = false;
			auto info = (WINDOWPOS*)lParam;

			// correct modern Windows styles invisible border
			RECT padding = inst->GetFrameSize(true);
			POINT pos = { info->x + padding.left, info->y + padding.top };
			if ((info->flags & SWP_NOMOVE) == 0)
			{
				if (pos.x != inst->windowPos.x || pos.y != inst->windowPos.y)
				{
					inst->windowPos = pos;
					updated = true;
				}
			}

			if ((info->flags & SWP_NOSIZE) == 0)
			{
				if (info->cx != inst->windowSize.x || info->cy != inst->windowSize.y)
				{
					inst->windowSize = { info->cx, info->cy };
					updated = true;
				}
			}

			if (updated)
			{
				inst->windowSizeClient = inst->ClientFromSize(inst->windowSize);
				if (inst->windowMode != WindowMode::Fullscreen && !IsZoomed(wnd))
				{
					inst->windowPosWindowed = inst->windowPos;
					inst->windowSizeWindowed = inst->windowSizeClient;
				}
				inst->WindowCalculateGeometry();
				inst->WindowUpdateTitle();
				inst->SaveConfig();
			}

			break;
		}
	}

	return CallWindowProc(inst->oriWindowProc, wnd, msg, wParam, lParam);
}

POINT WindowedMode::SizeFromClient(POINT clientSize) const
{
	auto frame = GetFrameSize();
	clientSize.x += frame.left + frame.right;
	clientSize.y += frame.top + frame.bottom;
	return clientSize;
}

POINT WindowedMode::ClientFromSize(POINT windowSize) const
{
	auto frame = GetFrameSize();
	windowSize.x -= frame.left + frame.right;
	windowSize.y -= frame.top + frame.bottom;
	return windowSize;
}

RECT WindowedMode::GetFrameSize(bool padOnly) const
{
	RECT frame = { 0 };

	if (padOnly)
	{
		RECT base, extended;
		if (GetWindowRect(window, &base) &&
			SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(RECT))))
		{
			frame.left = extended.left - base.left;
			frame.top = extended.top - base.top;
			frame.right = base.right - extended.right;
			frame.bottom = base.bottom - extended.bottom;
		}
	}
	else
	{
		AdjustWindowRectEx(&frame, WindowStyle(), false, WindowStyleEx());
		frame.left *= -1; // offset to thickness
		frame.top *= -1; // offset to thickness
	}

	return frame;
}

bool WindowedMode::IsD3D9() const
{
	return gameTitle == GameTitle::GTA_SA;
}

HRESULT WindowedMode::D3dPresentHook(IDirect3DDevice8* self, const RECT* srcRect, const RECT* dstRect, HWND wnd, const RGNDATA* region)
{
	inst->MouseUpdate();

	if (inst->fpsCounter.update())
		inst->WindowUpdateTitle();

	auto result = inst->d3dPresentOri(self, srcRect, dstRect, wnd, region);

	// limit framerate in main menu
	if (inst->menuFrameRateLimit > 0 && inst->IsMainMenuVisible())
	{
		static DWORD prevTime = 0;
		DWORD currTime = timeGetTime();
	
		while (true)
		{
			DWORD delta = currTime - prevTime;

			if (delta >= (1000 / (DWORD)inst->menuFrameRateLimit))
				break;

			Sleep(1);
			currTime = timeGetTime();
		}
		prevTime = currTime;
	}

	return result;
}

HRESULT WindowedMode::D3dResetHook(IDirect3DDevice8* self, D3DPRESENT_PARAMETERS* parameters)
{
	if (parameters->BackBufferWidth == inst->windowSizeClient.x && parameters->BackBufferHeight == inst->windowSizeClient.y)
	{
		inst->WindowCalculateGeometry(); // update presentation params
	}
	else // resolution changed
	{
		inst->WindowResize({ (LONG)parameters->BackBufferWidth, (LONG)parameters->BackBufferHeight });
	}

	auto result = inst->d3dResetOri(self, inst->d3dPresentParams8);

	if (SUCCEEDED(result))
		inst->UpdatePostEffect();

	return result;
}

bool WindowedMode::IsMainMenuVisible() const
{
	switch(gameTitle)
	{
		case GTA_3:
		{
			auto mgr = (CMenuManager3*)frontEndMenuManager;
			return mgr->m_bMenuActive;
		}

		case GTA_VC:
		{
			auto mgr = (CMenuManagerVC*)frontEndMenuManager;
			return mgr->m_bMenuActive;
		}
		
		case GTA_SA:
		{
			auto mgr = (CMenuManagerSA*)frontEndMenuManager;
			return mgr->m_bMenuActive;
		}

		default:
			return false;
	}
}

void WindowedMode::SwitchMainMenu(bool show)
{
	switch(gameTitle)
	{
		case GTA_3:
			if (show)
				injector::cstd<void()>::call(0x488770); // CMenuManager::RequestFrontEndStartUp()
			else
				injector::cstd<void()>::call(0x488750); // CMenuManager::RequestFrontEndShutDown()
			break;

		case GTA_VC:
		{
			auto mgr = (CMenuManagerVC*)frontEndMenuManager;
			if (show == mgr->m_bMenuActive) break; // already done
			
			mgr->m_bStartUpFrontEndRequested = show;
			mgr->m_bShutDownFrontEndRequested = !show;
			break;
		}
		
		case GTA_SA:
		{
			auto mgr = (CMenuManagerSA*)frontEndMenuManager;
			if (show == mgr->m_bMenuActive) break; // already done

			mgr->m_bActivateMenuNextFrame = show;
			mgr->m_bDontDrawFrontEnd = !show;
			break;
		}
	}
}

void WindowedMode::MouseUpdate(bool force)
{
	auto hasFocus = HasFocus(window);

	POINT pos;
	GetCursorPos(&pos);

	RECT rect;
	GetClientRect(window, &rect);
	ClientToScreen(window, (LPPOINT)&rect.left);
	ClientToScreen(window, (LPPOINT)&rect.right);

	// cursor visibility
	bool inGame = hasFocus && PtInRect(&rect, pos);
	SetCursorVisible(!inGame);

	// keep cursor inside the window (borderless/fullscreen only — plain windowed allows OS cursor escape)
	if (hasFocus || force)
	{
		if (!hasFocus || IsMainMenuVisible() || windowMode == WindowMode::Windowed)
			ClipCursor(NULL);
		else
			ClipCursor(&rect);
	}
}

void WindowedMode::UpdatePostEffect()
{
	switch(gameTitle)
	{
		case GameTitle::GTA_3:
			injector::cstd<void(RwCamera*)>::call(0x50AE40, *(RwCamera**)0x72676C); // CMBlurMotion::BlurOpen(RwCamera*)
			break;
			
		case GameTitle::GTA_VC:
			injector::cstd<void(RwCamera*)>::call(0x55CE20, *(RwCamera**)0x8100BC); // CMBlurMotion::BlurOpen(RwCamera*)
			break;
			
		case GameTitle::GTA_SA:
		{
			POINT oriSize;
			auto cam = *(RwCamera**)0xC1703C; // Scene.m_pRwCamera
			if (cam)
			{
				oriSize = { cam->frameBuffer->nWidth, cam->frameBuffer->nHeight }; // store
				cam->frameBuffer->nWidth = windowSizeClient.x;
				cam->frameBuffer->nHeight = windowSizeClient.y;
			}

			injector::cstd<void()>::call(0x7043D0); // CPostEffects::SetupBackBufferVertex()

			if (cam)
			{
				cam->frameBuffer->nWidth = oriSize.x; // restore
				cam->frameBuffer->nHeight = oriSize.y;
			}
			break;
		}
	}

	UpdateWidescreenFix();
}

void WindowedMode::UpdateWidescreenFix()
{
	static bool initialized = false;
	static HMODULE widescreenFix = NULL;
	static FARPROC updateFunc = NULL;

	if (!initialized)
	{
		switch(gameTitle)
		{
			case GameTitle::GTA_3:
				widescreenFix = GetModuleHandle("GTA3.WidescreenFix.asi");
				break;
			
			case GameTitle::GTA_VC:
				widescreenFix = GetModuleHandle("GTAVC.WidescreenFix.asi");
				break;
			
			case GameTitle::GTA_SA:
				widescreenFix = GetModuleHandle("GTASA.WidescreenFix.asi");
				break;
		}

		if (widescreenFix)
			updateFunc = GetProcAddress(widescreenFix, "UpdateVars");

		initialized = true;
	}

	if (updateFunc)
		injector::stdcall<void()>::call(updateFunc);
}

