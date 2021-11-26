#include "alienfx-gui.h"
#include <wtypes.h>
#include <windowsx.h>
#include <Shlobj.h>
#include <ColorDlg.h>
#include <wininet.h>
#include <Dbt.h>
#include "EventHandler.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib,"Version.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"Wininet.lib")

// Global Variables:
HINSTANCE hInst;
bool isNewVersion = false;
bool needUpdateFeedback = false;

HWND InitInstance(HINSTANCE, int);

BOOL CALLBACK DialogConfigStatic(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabColorDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabEventsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabDevicesDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabGroupsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabProfilesDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabSettingsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabFanDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabAmbientDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK TabHapticsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

FXHelper* fxhl;
ConfigHandler* conf;
EventHandler* eve;

// Fan control data
AlienFan_SDK::Control* acpi = NULL;             // ACPI control object

HWND mDlg = 0;

// Dialogues data....
// Common:
AlienFX_SDK::afx_act* mod;
HANDLE stopColorRefresh = 0;

HWND sTip = 0, lTip = 0;

// ConfigStatic:
int tabSel = -1;
UINT newTaskBar = RegisterWindowMessage(TEXT("TaskbarCreated"));

// last light selected
int eItem = -1;

DWORD EvaluteToAdmin() {
	// Evaluation attempt...
	DWORD dwError = ERROR_CANCELLED;
	char szPath[MAX_PATH];
	if (!IsUserAnAdmin()) {
		if (GetModuleFileName(NULL, szPath, ARRAYSIZE(szPath)))
		{
			// Launch itself as admin
			SHELLEXECUTEINFO sei{ sizeof(sei) };
			sei.lpVerb = "runas";
			sei.lpFile = szPath;
			sei.hwnd = NULL;
			sei.nShow = SW_NORMAL;
			if (!ShellExecuteEx(&sei))
			{
				dwError = GetLastError();
			}
			else
			{
				if (mDlg) {
					conf->Save();
					//fan_conf->Save();
					if (acpi)
						delete acpi;
					Shell_NotifyIcon(NIM_DELETE, &conf->niData);
					EndDialog(mDlg, 1);
				}
				_exit(1);  // Quit itself
			}
		}
		return dwError;
	}
	return 0;
}

bool DoStopService(bool kind) {
	SERVICE_STATUS_PROCESS ssp;
	ULONGLONG dwStartTime = GetTickCount64();
	DWORD dwBytesNeeded;
	DWORD dwTimeout = 10000; // 10-second time-out

	// Get a handle to the SCM database.

	SC_HANDLE schSCManager = OpenSCManager( NULL, NULL,GENERIC_READ);
	if (NULL == schSCManager)
	{
		//printf("OpenSCManager failed (%d)\n", GetLastError());
		return false;
	}

	// Get a handle to the service.

	SC_HANDLE schService = OpenService( schSCManager, "AWCCService",  SERVICE_QUERY_STATUS);

	if (schService == NULL)
	{
		CloseServiceHandle(schSCManager);
		return false;
	}

	// Make sure the service is not already stopped.

	if (QueryServiceStatusEx( schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
	{
		if (ssp.dwCurrentState == SERVICE_STOPPED)
		{
			if (kind) {
				CloseServiceHandle(schService);
				CloseServiceHandle(schSCManager);
				conf->block_power = false;
				return false;
			}
			else {
				schService = OpenService( schSCManager, "AWCCService", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);

				if (schService != NULL)
				{
					StartService( schService, 0, NULL);
					conf->block_power = true;
					CloseServiceHandle(schService);
					CloseServiceHandle(schSCManager);
					return false;
				}
			}
		}

		// Evalute UAC and re-open manager and service here.

		CloseServiceHandle(schService);

		if (conf->awcc_disable && kind) {
			schService = OpenService( schSCManager, "AWCCService", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);

			if (schService == NULL)
			{
				// Evaluation attempt...
				if (EvaluteToAdmin() == ERROR_CANCELLED)
				{
					CloseServiceHandle(schSCManager);
					conf->block_power = true;
				}
				return false;
			}

			// Send a stop code to the service.

			if (!ControlService(schService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp))
			{
				CloseServiceHandle(schService);
				CloseServiceHandle(schSCManager);
				return false;
			}

			// Wait for the service to stop.

			while (ssp.dwCurrentState != SERVICE_STOPPED)
			{
				Sleep(ssp.dwWaitHint);
				if (!QueryServiceStatusEx( schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
				{
					CloseServiceHandle(schService);
					CloseServiceHandle(schSCManager);
					return false;
				}

				if (ssp.dwCurrentState == SERVICE_STOPPED)
					break;

				if (GetTickCount64() - dwStartTime > dwTimeout)
				{
					CloseServiceHandle(schService);
					CloseServiceHandle(schSCManager);
					return false;
				}
			}
		}
		else conf->block_power = true;
	}

	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);
	return true;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	conf = new ConfigHandler();
	conf->Load();
	conf->SetStates();

	if (conf->activeProfile && conf->activeProfile->flags & PROF_FANS)
		conf->fan_conf->lastProf = &conf->activeProfile->fansets;

	// check fans...
	if (conf->fanControl) {
		EvaluteToAdmin();
		acpi = new AlienFan_SDK::Control();
		if (acpi->IsActivated()) {
			if (acpi->Probe()) {
				conf->fan_conf->SetBoosts(acpi);
				if (conf->fan_conf->lastProf->powerStage >= 0)
					acpi->SetPower(conf->fan_conf->lastProf->powerStage);
				if (conf->fan_conf->lastProf->GPUPower >= 0)
					acpi->SetGPU(conf->fan_conf->lastProf->GPUPower);
			} else {
				MessageBox(NULL, "Supported hardware not found. Fan control will be disabled!", "Error",
						   MB_OK | MB_ICONHAND);
				delete acpi;
				acpi = NULL;
				conf->fanControl = false;
			}
		} else {
			MessageBox(NULL, "Can't install fan driver, fan control will be disabled!", "Error",
					   MB_OK | MB_ICONHAND);
			delete acpi;
			acpi = NULL;
			conf->fanControl = false;
		}
	}

	fxhl = new FXHelper(conf);

	if (fxhl->FillAllDevs(acpi) || MessageBox(NULL, "No Alienware light devices detected!\nDo you want to continue?", "Error",
											MB_YESNO | MB_ICONWARNING) == IDYES) {
		conf->wasAWCC = DoStopService(true);

		if (conf->esif_temp)
			EvaluteToAdmin();

		fxhl->Start();

		eve = new EventHandler(conf, fxhl);
		eve->StartFanMon(acpi);
		eve->ChangePowerState();

		if (!(InitInstance(hInstance, conf->startMinimized ? SW_HIDE : SW_NORMAL)))
			return FALSE;

		//register global hotkeys...
		RegisterHotKey( mDlg, 1, MOD_CONTROL | MOD_SHIFT, VK_F12 );
		RegisterHotKey( mDlg, 2, MOD_CONTROL | MOD_SHIFT, VK_F11 );
		RegisterHotKey( mDlg, 4, MOD_CONTROL | MOD_SHIFT, VK_F10 );
		RegisterHotKey( mDlg, 3, 0, VK_F18 );
		RegisterHotKey( mDlg, 5, MOD_CONTROL | MOD_SHIFT, VK_F9 );
		//effect change hotkeys...
		for (int i = 0; i < 9; i++)
			RegisterHotKey(mDlg, 10+i, MOD_CONTROL | MOD_SHIFT, 0x31 + i);

		// Power notifications...
		RegisterPowerSettingNotification(mDlg, &GUID_MONITOR_POWER_ON, 0);

		HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_ALIENFXGUI));

		MSG msg;
		// Main message loop:
		while ((GetMessage(&msg, 0, 0, 0)) != 0) {
			if (!TranslateAccelerator(mDlg, hAccelTable, &msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

		delete eve;
		fxhl->Stop();
		fxhl->afx_dev.SaveMappings();

		if (conf->wasAWCC) DoStopService(false);
	}

	if (acpi) {
		delete acpi;
	}

	delete fxhl;
	delete conf;

	return 0;
}

HWND InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hInst = hInstance;
	CreateDialogParam(hInstance, MAKEINTRESOURCE(IDD_MAINWINDOW), NULL, (DLGPROC)DialogConfigStatic, 0);

	if (mDlg) {

		SendMessage(mDlg, WM_SETICON, ICON_BIG, (LPARAM) LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ALIENFXGUI)));
		SendMessage(mDlg, WM_SETICON, ICON_SMALL, (LPARAM) LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ALIENFXGUI), IMAGE_ICON, 16, 16, 0));

		ShowWindow(mDlg, nCmdShow);
	}
	return mDlg;
}

int UpdateLightList(HWND light_list, FXHelper* fxhl, int flag = 0) {
	int pos = -1, selpos = -1;
	size_t lights = fxhl->afx_dev.GetMappings()->size();
	size_t groups = fxhl->afx_dev.GetGroups()->size();
	
	ListBox_ResetContent(light_list);

	for (int i = 0; i < groups; i++) {
		AlienFX_SDK::group grp = fxhl->afx_dev.GetGroups()->at(i);
		string fname = grp.name + " (Group)";
		pos = ListBox_AddString(light_list, fname.c_str());
		ListBox_SetItemData(light_list, pos, grp.gid);
		if (grp.gid == eItem)
			ListBox_SetCurSel(light_list, selpos = pos);
	}
	for (int i = 0; i < lights; i++) {
		AlienFX_SDK::mapping* lgh = fxhl->afx_dev.GetMappings()->at(i);
		if (fxhl->LocateDev(lgh->devid) && !(lgh->flags & flag)) {
			pos = ListBox_AddString(light_list, lgh->name.c_str());
			ListBox_SetItemData(light_list, pos, i);
			if (i == eItem)
				ListBox_SetCurSel(light_list, selpos = pos);
		}
	}
	if (selpos < 0) {
		eItem = -1;
	}
	return lights > 0 ? pos : -1;
}

void RedrawButton(HWND hDlg, unsigned id, AlienFX_SDK::afx_act* act) {
	RECT rect;
	HBRUSH Brush = NULL;
	HWND tl = GetDlgItem(hDlg, id);
	GetWindowRect(tl, &rect);
	HDC cnt = GetWindowDC(tl);
	rect.bottom -= rect.top;
	rect.right -= rect.left;
	rect.top = rect.left = 0;
	// BGR!
	Brush = CreateSolidBrush(RGB(act->r, act->g, act->b));
	FillRect(cnt, &rect, Brush);
	DrawEdge(cnt, &rect, EDGE_RAISED, BF_RECT);
	DeleteObject(Brush);
	ReleaseDC(tl, cnt);
}

HWND CreateToolTip(HWND hwndParent, HWND oldTip)
{
	// Create a tooltip.
	if (oldTip) {
		DestroyWindow(oldTip);
	} 
	HWND hwndTT = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
								 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
								 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
								 hwndParent, NULL, hInst, NULL);

	SetWindowPos(hwndTT, HWND_TOPMOST, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	TOOLINFO ti{ 0 };
	ti.cbSize = sizeof(TOOLINFO);
	ti.uFlags = TTF_SUBCLASS;
	ti.hwnd = hwndParent;
	ti.hinst = hInst;
	ti.lpszText = (LPTSTR)"0";

	GetClientRect(hwndParent, &ti.rect);

	SendMessage(hwndTT, TTM_ADDTOOL, 0, (LPARAM)(LPTOOLINFO)&ti);
	return hwndTT;
}

void SetSlider(HWND tt, int value) {
	TOOLINFO ti{0};
	ti.cbSize = sizeof(ti);
	if (tt) {
		SendMessage(tt, TTM_ENUMTOOLS, 0, (LPARAM) &ti);
		string toolTip = to_string(value);
		ti.lpszText = (LPTSTR) toolTip.c_str();
		SendMessage(tt, TTM_SETTOOLINFO, 0, (LPARAM) &ti);
	}

}

string GetAppVersion() {

	HRSRC hResInfo = FindResource(hInst, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);

	string res;

	if (hResInfo) {
		DWORD dwSize = SizeofResource(hInst, hResInfo);
		HGLOBAL hResData = LoadResource(hInst, hResInfo);
		if (hResData) {
			LPVOID pRes = LockResource(hResData),
				pResCopy = LocalAlloc(LMEM_FIXED, dwSize);
			if (pResCopy) {
				UINT uLen = 0;
				VS_FIXEDFILEINFO *lpFfi = NULL;

				CopyMemory(pResCopy, pRes, dwSize);

				VerQueryValue(pResCopy, TEXT("\\"), (LPVOID*)&lpFfi, &uLen);

				res = to_string(HIWORD(lpFfi->dwFileVersionMS)) + "."
					+ to_string(LOWORD(lpFfi->dwFileVersionMS)) + "."
					+ to_string(HIWORD(lpFfi->dwFileVersionLS)) + "."
					+ to_string(LOWORD(lpFfi->dwFileVersionLS));

				LocalFree(pResCopy);
			}
			FreeResource(hResData);
		}
	}
	return res;
}

DWORD WINAPI CUpdateCheck(LPVOID lparam) {
	NOTIFYICONDATA* niData = (NOTIFYICONDATA*) lparam;
	HINTERNET session, req;
	char buf[2048];
	DWORD byteRead;
	// Wait connection for a while
	if (!needUpdateFeedback)
		Sleep(10000);
	if (session = InternetOpen("alienfx-tools", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0)) {
		if (req = InternetOpenUrl(session, "https://api.github.com/repos/t-troll/alienfx-tools/tags?per_page=1",
								  NULL, 0, 0, NULL)) {
			if (InternetReadFile(req, buf, 2047, &byteRead)) {
				buf[byteRead] = 0;
				string res = buf;
				size_t pos = res.find("\"name\":"), 
					posf = res.find("\"", pos + 8);
				res = res.substr(pos + 8, posf - pos - 8);
				size_t dotpos = res.find(".", 1+ res.find(".", 1 + res.find(".")));
				if (res.find(".", 1+ res.find(".", 1 + res.find("."))) == string::npos)
					res += ".0";
				if (res != GetAppVersion()) {
					// new version detected!
					niData->uFlags |= NIF_INFO;
					strcpy_s(niData->szInfoTitle, "Update avaliable!");
					strcpy_s(niData->szInfo, ("Version " + res + " released at GitHub.").c_str());
					Shell_NotifyIcon(NIM_MODIFY, niData);
					niData->uFlags &= ~NIF_INFO;
					isNewVersion = true;
				}
			}
			InternetCloseHandle(req);
		}
		InternetCloseHandle(session);
	}
	if (!isNewVersion && needUpdateFeedback) {
		niData->uFlags |= NIF_INFO;
		strcpy_s(niData->szInfoTitle, "You are up to date!");
		strcpy_s(niData->szInfo, "You are using latest version.");
		Shell_NotifyIcon(NIM_MODIFY, niData);
		niData->uFlags &= ~NIF_INFO;
	}
	return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG: {

		Static_SetText(GetDlgItem(hDlg, IDC_STATIC_VERSION), ("Version: " + GetAppVersion()).c_str());

		return (INT_PTR)TRUE;
	} break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK: case IDCANCEL:
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		} break;
		}
		break;
	case WM_NOTIFY:
		switch (LOWORD(wParam)) {
		case IDC_SYSLINK_HOMEPAGE:
			switch (((LPNMHDR)lParam)->code)
			{
			case NM_CLICK:
			case NM_RETURN:
			{
				char hurl[MAX_PATH];
				LoadString(hInst, IDS_HOMEPAGE, hurl, MAX_PATH);
				ShellExecute(NULL, "open", hurl, NULL, NULL, SW_SHOWNORMAL);
			} break;
			} break;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

DLGTEMPLATE* DoLockDlgRes(LPCTSTR lpszResName)
{
	HRSRC hrsrc = FindResource(NULL, lpszResName, RT_DIALOG);

	HGLOBAL hglb = LoadResource(hInst, hrsrc);
	return (DLGTEMPLATE*)LockResource(hglb);
}

VOID OnSelChanged(HWND hwndDlg)
{
	// Get the dialog header data.
	DLGHDR* pHdr = (DLGHDR*)GetWindowLongPtr(
		hwndDlg, GWLP_USERDATA);

	// Get the index of the selected tab.
	tabSel = TabCtrl_GetCurSel(pHdr->hwndTab);

	// Destroy the current child dialog box, if any.
	if (pHdr->hwndDisplay != NULL) {
		EndDialog(pHdr->hwndDisplay, IDOK);
		DestroyWindow(pHdr->hwndDisplay);
		pHdr->hwndDisplay = NULL;
	}
	DLGPROC tdl = NULL;
	switch (tabSel) {
	case TAB_COLOR: tdl = (DLGPROC)TabColorDialog; break;
	case TAB_EVENTS: tdl = (DLGPROC)TabEventsDialog; break;
	case TAB_AMBIENT: tdl = (DLGPROC)TabAmbientDialog; break;
	case TAB_HAPTICS: tdl = (DLGPROC)TabHapticsDialog; break;
	case TAB_GROUPS: tdl = (DLGPROC)TabGroupsDialog; break;
	case TAB_PROFILES: tdl = (DLGPROC)TabProfilesDialog; break;
	case TAB_DEVICES: tdl = (DLGPROC)TabDevicesDialog; break;
	case TAB_FANS: tdl = (DLGPROC)TabFanDialog; break;
	case TAB_SETTINGS: tdl = (DLGPROC)TabSettingsDialog; break;
	//default: tdl = (DLGPROC)TabColorDialog;
	}

	HWND newDisplay = CreateDialogIndirect(hInst, (DLGTEMPLATE*)pHdr->apRes[tabSel], pHdr->hwndTab, tdl);
	if (pHdr->hwndDisplay == NULL)
		pHdr->hwndDisplay = newDisplay;

	if (pHdr->hwndDisplay != NULL)
		SetWindowPos(pHdr->hwndDisplay, NULL,
			pHdr->rcDisplay.left, pHdr->rcDisplay.top,
			(pHdr->rcDisplay.right - pHdr->rcDisplay.left), 
			(pHdr->rcDisplay.bottom - pHdr->rcDisplay.top),
			SWP_SHOWWINDOW);
	return;
}

void SwitchTab(int num) {
	HWND tab_list = GetDlgItem(mDlg, IDC_TAB_MAIN);
	TabCtrl_SetCurSel(tab_list, num);
	OnSelChanged(tab_list);
}

void ReloadProfileList() {
	HWND tab_list = GetDlgItem(mDlg, IDC_TAB_MAIN), 
		profile_list = GetDlgItem(mDlg, IDC_PROFILES),
		mode_list = GetDlgItem(mDlg, IDC_EFFECT_MODE);
	ComboBox_ResetContent(profile_list);
	for (int i = 0; i < conf->profiles.size(); i++) {
		int pos = ComboBox_AddString(profile_list, conf->profiles[i].name.c_str());
		ComboBox_SetItemData(profile_list, pos, conf->profiles[i].id);
		if (conf->profiles[i].id == conf->activeProfile->id) {
			ComboBox_SetCurSel(profile_list, pos);
			ComboBox_SetCurSel(mode_list, conf->profiles[i].effmode);
		}
	}

	EnableWindow(profile_list, !conf->enableProf);
	EnableWindow(mode_list, conf->enableMon);

	switch (tabSel) {
	case TAB_COLOR: case TAB_EVENTS: case TAB_SETTINGS: case TAB_FANS:
		OnSelChanged(tab_list);
	}
}

void ReloadModeList(HWND mode_list = NULL, int mode = conf->GetEffect()) {
	if (mode_list == NULL)
		mode_list = GetDlgItem(mDlg, IDC_EFFECT_MODE);
	ListBox_ResetContent(mode_list);
	ComboBox_AddString(mode_list, "Monitoring");
	ComboBox_AddString(mode_list, "Ambient");
	ComboBox_AddString(mode_list, "Haptics");
	ComboBox_AddString(mode_list, "Off");
	ComboBox_SetCurSel(mode_list, mode);
	EnableWindow(mode_list, conf->enableMon);
}

void UpdateState() {
	HWND tab_list = GetDlgItem(mDlg, IDC_TAB_MAIN);
	fxhl->ChangeState();
	if (tabSel == TAB_SETTINGS)
		OnSelChanged(tab_list);
}

BOOL CALLBACK DialogConfigStatic(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	HWND tab_list = GetDlgItem(hDlg, IDC_TAB_MAIN),
		profile_list = GetDlgItem(hDlg, IDC_PROFILES),
		mode_list = GetDlgItem(hDlg, IDC_EFFECT_MODE);

	if (message == newTaskBar) {
		// Started/restarted explorer...
		Shell_NotifyIcon(NIM_ADD, &conf->niData);
		CreateThread(NULL, 0, CUpdateCheck, &conf->niData, 0, NULL);
		return true;
	}

	switch (message)
	{
	case WM_INITDIALOG:
	{
		mDlg = hDlg;
		DLGHDR *pHdr = (DLGHDR *) LocalAlloc(LPTR, sizeof(DLGHDR));
		SetWindowLongPtr(tab_list, GWLP_USERDATA, (LONG_PTR)pHdr);

		pHdr->hwndTab = tab_list;

		TCITEM tie{0};
		char nBuf[64]{0};

		tie.mask = TCIF_TEXT;
		tie.iImage = -1;
		tie.pszText = nBuf;

		for (int i = 0; i < C_PAGES; i++) {
			pHdr->apRes[i] = DoLockDlgRes(MAKEINTRESOURCE((IDD_DIALOG_COLORS + i)));
			LoadString(hInst, IDS_TAB_COLOR + i, tie.pszText, 64);
			SendMessage(tab_list, TCM_INSERTITEM, i, (LPARAM) &tie);
		}

		GetClientRect(pHdr->hwndTab, &pHdr->rcDisplay);

		pHdr->rcDisplay.left = GetSystemMetrics(SM_CXBORDER);
		pHdr->rcDisplay.top = GetSystemMetrics(SM_CYSMSIZE) - GetSystemMetrics(SM_CYBORDER);
		pHdr->rcDisplay.right -= 2 * GetSystemMetrics(SM_CXBORDER) + 1;
		pHdr->rcDisplay.bottom -= GetSystemMetrics(SM_CYBORDER) + 1;

		ReloadModeList();

		ReloadProfileList();

		OnSelChanged(tab_list);

		conf->niData.hWnd = hDlg;
		conf->SetIconState();
		
		if (Shell_NotifyIcon(NIM_ADD, &conf->niData)) {
			// check update....
			CreateThread(NULL, 0, CUpdateCheck, &conf->niData, 0, NULL);
		} 

		conf->SetStates();
	} break;
	case WM_COMMAND:
	{
		if (LOWORD(wParam) >= ID_ACC_COLOR && LOWORD(wParam) <= ID_ACC_SETTINGS) {
			SwitchTab(TAB_COLOR + LOWORD(wParam) - ID_ACC_COLOR);
			break;
		}
		switch (LOWORD(wParam))
		{
		case IDOK: case IDCANCEL: case IDCLOSE: case IDM_EXIT:
		{
			SendMessage(hDlg, WM_CLOSE, 0, 0);
		} break;
		case IDM_ABOUT:
			DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hDlg, About);
			break;
		case IDM_HELP:
			ShellExecute(NULL, "open", "https://github.com/T-Troll/alienfx-tools/blob/master/Doc/alienfx-gui.md", NULL, NULL, SW_SHOWNORMAL);
			break;
		case IDM_CHECKUPDATE:
			// check update....
			needUpdateFeedback = true;
			CreateThread(NULL, 0, CUpdateCheck, &conf->niData, 0, NULL);
			break;
		case IDC_BUTTON_MINIMIZE:
			SendMessage(hDlg, WM_SIZE, SIZE_MINIMIZED, 0);
			break;
		case IDC_BUTTON_REFRESH:
			fxhl->RefreshState(true);
			ReloadProfileList();
			break;
		case IDC_BUTTON_SAVE:
			fxhl->afx_dev.SaveMappings();
			conf->Save();
			isNewVersion = false;
			fxhl->Refresh(2); // set def. colors
			conf->niData.uFlags |= NIF_INFO;
			strcpy_s(conf->niData.szInfoTitle, "Configuration saved!");
			strcpy_s(conf->niData.szInfo, "Configuration saved succesdully.");
			Shell_NotifyIcon(NIM_MODIFY, &conf->niData);
			conf->niData.uFlags &= ~NIF_INFO;
			break;
		case IDC_EFFECT_MODE:
		{
			switch (HIWORD(wParam)) {
			case CBN_SELCHANGE:
			{
				int effmode = conf->activeProfile->effmode = ComboBox_GetCurSel(mode_list);
				eve->ChangeEffectMode(effmode);
			} break;
			}
		} break;
		case IDC_PROFILES: {
			switch (HIWORD(wParam))
			{
			case CBN_SELCHANGE: {
				int prid = (int)ComboBox_GetItemData(profile_list, ComboBox_GetCurSel(profile_list));
				eve->SwitchActiveProfile(conf->FindProfile(prid));
				ReloadProfileList();
			} break;
			}
		} break;
		} break;
	} break;
	case WM_NOTIFY: {
		switch (((NMHDR*)lParam)->idFrom) {
		case IDC_TAB_MAIN: {
			if (((NMHDR*)lParam)->code == TCN_SELCHANGE) {
				//int newTab = TabCtrl_GetCurSel(tab_list);
				if (TabCtrl_GetCurSel(tab_list) != tabSel) { // selection changed!
					OnSelChanged(tab_list);
				}
			}
		} break;
		}
	} break;
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED) {
			// go to tray...
			if (!fxhl->unblockUpdates) {
				fxhl->UnblockUpdates(true, true);
				fxhl->RefreshState();
			}
			ShowWindow(hDlg, SW_HIDE);
		} break;
	case WM_APP + 1: {
		switch (lParam)
		{
		case WM_LBUTTONDBLCLK:
		case WM_LBUTTONUP:
			ShowWindow(hDlg, SW_RESTORE);
			SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
			SetWindowPos(hDlg, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
			ReloadProfileList();
			break;
		case WM_RBUTTONUP: case WM_CONTEXTMENU:
		{
			POINT lpClickPoint;
			HMENU tMenu = LoadMenuA(hInst, MAKEINTRESOURCEA(IDR_MENU_TRAY));
			tMenu = GetSubMenu(tMenu, 0);
			MENUINFO mi;
			memset(&mi, 0, sizeof(mi));
			mi.cbSize = sizeof(mi);
			mi.fMask = MIM_STYLE;
			mi.dwStyle = MNS_NOTIFYBYPOS;
			SetMenuInfo(tMenu, &mi);
			MENUITEMINFO mInfo{0};
			mInfo.cbSize = sizeof(MENUITEMINFO);
			mInfo.fMask = MIIM_STRING | MIIM_ID;
			HMENU pMenu;
			// add profiles...
			if (!conf->enableProf) {
				pMenu = CreatePopupMenu();
				mInfo.wID = ID_TRAYMENU_PROFILE_SELECTED;
				for (int i = 0; i < conf->profiles.size(); i++) {
					mInfo.dwTypeData = (LPSTR) conf->profiles[i].name.c_str();
					InsertMenuItem(pMenu, i, false, &mInfo);
					if (conf->profiles[i].id == conf->activeProfile->id)
						CheckMenuItem(pMenu, i, MF_BYPOSITION | MF_CHECKED);
				}
				ModifyMenu(tMenu, ID_TRAYMENU_PROFILES, MF_BYCOMMAND | MF_STRING | MF_POPUP, (UINT_PTR) pMenu, "Profiles...");
			} else
				ModifyMenu(tMenu, ID_TRAYMENU_PROFILES, MF_STRING, NULL, ("Profile - " + conf->activeProfile->name).c_str());
			// add effects menu...
			if (conf->enableMon) {
				pMenu = CreatePopupMenu();
				mInfo.wID = ID_TRAYMENU_MONITORING_SELECTED;
				mInfo.dwTypeData = "Monitoring";
				InsertMenuItem(pMenu, 0, false, &mInfo);
				mInfo.dwTypeData = "Ambient";
				InsertMenuItem(pMenu, 1, false, &mInfo);
				mInfo.dwTypeData = "Haptics";
				InsertMenuItem(pMenu, 2, false, &mInfo);
				mInfo.dwTypeData = "Off";
				InsertMenuItem(pMenu, 3, false, &mInfo);
				CheckMenuItem(pMenu, conf->GetEffect(), MF_BYPOSITION | MF_CHECKED);
				ModifyMenu(tMenu, ID_TRAYMENU_MONITORING, MF_BYCOMMAND | MF_POPUP | MF_STRING, (UINT_PTR) pMenu, "Effects...");
			}

			CheckMenuItem(tMenu, ID_TRAYMENU_ENABLEEFFECTS, conf->enableMon ? MF_CHECKED : MF_UNCHECKED);
			CheckMenuItem(tMenu, ID_TRAYMENU_LIGHTSON, conf->lightsOn ? MF_CHECKED : MF_UNCHECKED);
			CheckMenuItem(tMenu, ID_TRAYMENU_DIMLIGHTS, conf->IsDimmed() ? MF_CHECKED : MF_UNCHECKED);
			CheckMenuItem(tMenu, ID_TRAYMENU_PROFILESWITCH, conf->enableProf? MF_CHECKED : MF_UNCHECKED);

			GetCursorPos(&lpClickPoint);
			SetForegroundWindow(hDlg);
			TrackPopupMenu(tMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_BOTTOMALIGN,
				lpClickPoint.x, lpClickPoint.y, 0, hDlg, NULL);
		} break;
		case NIN_BALLOONHIDE : case NIN_BALLOONTIMEOUT:
			if (!isNewVersion) {
				Shell_NotifyIcon(NIM_DELETE, &conf->niData);
				Shell_NotifyIcon(NIM_ADD, &conf->niData);
			} else
				isNewVersion = false;
			break;
		case NIN_BALLOONUSERCLICK:
		{
			if (isNewVersion) {
				char uurl[MAX_PATH];
				LoadString(hInst, IDS_UPDATEPAGE, uurl, MAX_PATH);
				ShellExecute(NULL, "open", uurl, NULL, NULL, SW_SHOWNORMAL);
				isNewVersion = false;
			} 
		} break;
		}
		break;
	} break;
	case WM_MENUCOMMAND: {
		int idx = LOWORD(wParam);
		switch (GetMenuItemID((HMENU)lParam, idx)) {
		case ID_TRAYMENU_EXIT:
			SendMessage(hDlg, WM_CLOSE, 0, 0);
		    return true;
		case ID_TRAYMENU_REFRESH:
			fxhl->RefreshState(true);
			break;
		case ID_TRAYMENU_LIGHTSON:
			conf->lightsOn = !conf->lightsOn;
			eve->ToggleEvents();
			UpdateState();
			break;
		case ID_TRAYMENU_DIMLIGHTS:
		    conf->SetDimmed();
			UpdateState();
			break;
		case ID_TRAYMENU_ENABLEEFFECTS:
			conf->enableMon = !conf->enableMon;
			eve->ToggleEvents();
			break;
		case ID_TRAYMENU_MONITORING_SELECTED:
			conf->activeProfile->effmode = idx;
			eve->ChangeEffectMode(idx);
			break;
		case ID_TRAYMENU_PROFILESWITCH:
			eve->StopProfiles();
			conf->enableProf = !conf->enableProf;
			eve->StartProfiles();
			break;
		case ID_TRAYMENU_RESTORE:
			ShowWindow(hDlg, SW_RESTORE);
			SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
			SetWindowPos(hDlg, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
			break;
		case ID_TRAYMENU_PROFILE_SELECTED: {
			if (conf->profiles[idx].id != conf->activeProfile->id) {
				eve->SwitchActiveProfile(&conf->profiles[idx]);
			}
		} break;
		}
		ReloadProfileList();
	} break;
	case WM_POWERBROADCAST:
		switch (wParam) {
		case PBT_APMRESUMEAUTOMATIC: {
			// resume from sleep/hybernate

			DebugPrint("Resume from Sleep/hibernate initiated\n");

			if (fxhl->FillAllDevs(acpi)) {
				fxhl->UnblockUpdates(true);
				eve->ChangePowerState();
				conf->stateScreen = true;
				eve->ToggleEvents();
				eve->StartProfiles();
			}
			if (eve->mon)
				eve->mon->Start();
			CreateThread(NULL, 0, CUpdateCheck, &conf->niData, 0, NULL);
		} break;
		case PBT_APMPOWERSTATUSCHANGE:
			// ac/batt change
			eve->ChangePowerState();
			break;
		case PBT_POWERSETTINGCHANGE: {
			POWERBROADCAST_SETTING* sParams = (POWERBROADCAST_SETTING*) lParam;
			if (sParams->PowerSetting == GUID_MONITOR_POWER_ON || sParams->PowerSetting == GUID_CONSOLE_DISPLAY_STATE
				|| sParams->PowerSetting == GUID_SESSION_DISPLAY_STATUS) {
				eve->ChangeScreenState(sParams->Data[0]);
			}
		} break;
		case PBT_APMSUSPEND:
			// Sleep initiated.

			DebugPrint("Sleep/hibernate initiated\n");

			conf->stateScreen = true;
			fxhl->ChangeState();
			eve->StopProfiles();
			eve->StopEffects();
			fxhl->Refresh(2);
			fxhl->UnblockUpdates(false);
			if (eve->mon)
				eve->mon->Stop();
			break;
		}
		break;
	case WM_DEVICECHANGE:
		if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
			vector<pair<WORD, WORD>> devs = fxhl->afx_dev.AlienFXEnumDevices();
			if (devs.size() != fxhl->afx_dev.fxdevs.size()) {
				// Device added or removed, need to rescan devices...
				bool wasNotLocked = !fxhl->updateLock;
				if (wasNotLocked) {
					fxhl->UnblockUpdates(false, true);
				}
				fxhl->FillAllDevs(acpi);
				if (wasNotLocked)
					fxhl->UnblockUpdates(true, true);
			}
		}
		break;
	case WM_QUERYENDSESSION:
		return true;
	case WM_DISPLAYCHANGE:
		// Monitor configuration changed
	    if (eve->capt) eve->capt->Restart();
	break;
	case WM_ENDSESSION:
		// Shutdown/restart scheduled....

		DebugPrint("Shutdown initiated\n");

		conf->Save();
		eve->StopProfiles();
		eve->StopEffects();
		fxhl->Refresh(2);
		fxhl->UnblockUpdates(false, true);
		return 0;
		break;
	case WM_HOTKEY:
		switch (wParam) {
		case 1: // on/off
			conf->lightsOn = !conf->lightsOn;
			UpdateState();
			break;
		case 2: // dim
		    conf->SetDimmed();
			UpdateState();
			break;
		case 3: // off-dim-full circle
			if (conf->lightsOn) {
				if (conf->IsDimmed())
					conf->lightsOn = false;
				conf->SetDimmed();
			} else
				conf->lightsOn = true;
			UpdateState();
			break;
		case 4: // effects
			conf->enableMon = !conf->enableMon;
			eve->ToggleEvents();
			ComboBox_SetCurSel(mode_list, conf->GetEffect());
			break;
		case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: // profile switch
			if (wParam - 10 < conf->profiles.size()) {
				eve->SwitchActiveProfile(&conf->profiles[wParam - 10]);
				ReloadProfileList();
			}
			break;
		case 5: // profile autoswitch
			eve->StopProfiles();
			conf->enableProf = !conf->enableProf;
			eve->StartProfiles();
			ReloadProfileList();
			break;
		default: return false;
		}
		break;
	case WM_CLOSE:
		fxhl->Refresh(2);
		Shell_NotifyIcon(NIM_DELETE, &conf->niData);
		EndDialog(hDlg, IDOK);
		DestroyWindow(hDlg);
		return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0); break;
	default: return false;
	}
	return true;
}

DWORD CColorRefreshProc(LPVOID param) {
	AlienFX_SDK::afx_act last = *mod;
	lightset* mmap = (lightset*)param;
	while (WaitForSingleObject(stopColorRefresh, 250)) {
		if (last.r != mod->r || last.g != mod->g || last.b != mod->b) {
			// set colors...
			last = *mod;
			if (mmap) {
				fxhl->RefreshOne(mmap, 0, true);
			}
		}
	}
	return 0;
}

UINT_PTR Lpcchookproc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	DRAWITEMSTRUCT* item = 0;
	//UINT r = 0, g = 0, b = 0;

	switch (message)
	{
	case WM_INITDIALOG:
		mod = (AlienFX_SDK::afx_act*)((CHOOSECOLOR*)lParam)->lCustData;
		break;
	case WM_CTLCOLOREDIT:
		mod->r = GetDlgItemInt(hDlg, COLOR_RED, NULL, false);
		mod->g = GetDlgItemInt(hDlg, COLOR_GREEN, NULL, false);
		mod->b = GetDlgItemInt(hDlg, COLOR_BLUE, NULL, false);
		//if (r != mod->r || g != mod->g || b != mod->b) {
		//	mod->r = r;
		//	mod->g = g;
		//	mod->b = b;
		//}
		break;
	}
	return 0;
}

bool SetColor(HWND hDlg, int id, lightset* mmap, AlienFX_SDK::afx_act* map) {
	CHOOSECOLOR cc{0};
	bool ret;

	AlienFX_SDK::afx_act savedColor = *map;
	DWORD crThreadID;
	HANDLE crRefresh;

	// Initialize CHOOSECOLOR
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = hDlg;
	cc.lpfnHook = Lpcchookproc;
	cc.lCustData = (LPARAM)map;
	cc.lpCustColors = (LPDWORD)conf->customColors;
	cc.rgbResult = RGB(map->r, map->g, map->b);
	cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_ENABLEHOOK;

	mod = map;
	stopColorRefresh = CreateEvent(NULL, false, false, NULL);
	crRefresh = CreateThread(NULL, 0, CColorRefreshProc, mmap, 0, &crThreadID);

	ret = ChooseColor(&cc);

	SetEvent(stopColorRefresh);
	WaitForSingleObject(crRefresh, 1200);
	CloseHandle(crRefresh);
	CloseHandle(stopColorRefresh);

	if (!ret)
	{
		(*map) = savedColor;
		fxhl->RefreshOne(mmap, true, true);
	}
	RedrawButton(hDlg, id, map);
	return ret;
}

bool SetColor(HWND hDlg, int id, Colorcode *clr) {
	CHOOSECOLOR cc{0};
	bool ret;
	// Initialize CHOOSECOLOR 
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = hDlg;
	cc.lpCustColors = (LPDWORD) conf->customColors;
	cc.rgbResult = RGB(clr->r, clr->g, clr->b);
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;

	if (ret = ChooseColor(&cc)) {
		clr->r = cc.rgbResult & 0xff;
		clr->g = cc.rgbResult >> 8 & 0xff;
		clr->b = cc.rgbResult >> 16 & 0xff;
	}
	AlienFX_SDK::afx_act act{0,0,0,clr->r,clr->g,clr->g};
	RedrawButton(hDlg, id, &act);
	return ret;
}

lightset* FindMapping(int mid)
{
	if (!(mid < 0)) {
		if (mid > 0xffff) {
			// group
			for (int i = 0; i < conf->active_set->size(); i++)
				if (conf->active_set->at(i).devid == 0 && conf->active_set->at(i).lightid == mid) {
					return &conf->active_set->at(i);
				}
		} else {
			// mapping
			if (fxhl->afx_dev.GetMappings()->size() > mid) {
				AlienFX_SDK::mapping* lgh = fxhl->afx_dev.GetMappings()->at(mid);
				for (int i = 0; i < conf->active_set->size(); i++)
					if (conf->active_set->at(i).devid == lgh->devid && 
						conf->active_set->at(i).lightid == lgh->lightid) {
						return &conf->active_set->at(i);
					}
			}
		}
	}
	return nullptr;
}

lightset* CreateMapping(int lid) {
	// create new mapping..
	lightset newmap;
	AlienFX_SDK::afx_act act{0};
	if (lid > 0xffff) {
		// group
		newmap = {0,0,(unsigned)lid};
	} else {
		// light
		AlienFX_SDK::mapping* lgh = fxhl->afx_dev.GetMappings()->at(lid);
		newmap = {lgh->devid, 0/*lgh->vid*/, lgh->lightid};
		if (lgh->flags & ALIENFX_FLAG_POWER) {
			act.type = AlienFX_SDK::AlienFX_A_Power;
			act.time = 3;
			act.tempo = 0x64;
			newmap.eve[0].map.push_back(act);
		}
	}
	newmap.eve[0].fs.b.flags = 1;
	newmap.eve[0].map.push_back(act);
	newmap.eve[1].map.push_back(act);
	newmap.eve[1].map.push_back(act);
	newmap.eve[2].map.push_back(act);
	newmap.eve[2].map.push_back(act);
	newmap.eve[2].fs.b.cut = 0;
	newmap.eve[3].map.push_back(act);
	newmap.eve[3].map.push_back(act);
	newmap.eve[3].fs.b.cut = 90;
	conf->active_set->push_back(newmap);
	return FindMapping(lid);
}

bool RemoveMapping(vector<lightset>* lightsets, int did, int lid) {
	// erase mappings
	for (vector <lightset>::iterator mIter = lightsets->begin();
		mIter != lightsets->end(); mIter++)
		if (mIter->devid == did && mIter->lightid == lid) {
			lightsets->erase(mIter);
			return true;
		}
	return false;
}
