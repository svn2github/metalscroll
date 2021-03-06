/*
*	Copyright 2009 Griffin Software
*
*	Licensed under the Apache License, Version 2.0 (the "License");
*	you may not use this file except in compliance with the License.
*	You may obtain a copy of the License at
*
*		http://www.apache.org/licenses/LICENSE-2.0
*
*	Unless required by applicable law or agreed to in writing, software
*	distributed under the License is distributed on an "AS IS" BASIS,
*	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*	See the License for the specific language governing permissions and
*	limitations under the License. 
*/

#include "MetalScrollPCH.h"
#include "Connect.h"
#include "MetalBar.h"
#include "MarkerGUID.h"
#include "Utils.h"

extern CAddInModule _AtlModule;

CComPtr<EnvDTE80::DTE2>				g_dte;
CComPtr<EnvDTE::AddIn>				g_addInInstance;
CComPtr<IVsTextManager>				g_textMgr;
long								g_highlightMarkerType;
HWND								g_mainVSHwnd = 0;

CConnect::CConnect()
{
	m_textMgrEventsCookie = 0;
}

bool CConnect::GetTextManagerEventsPlug(IConnectionPoint** connPt)
{
	CComQIPtr<IConnectionPointContainer> connPoints = g_textMgr;
	if(!connPoints)
		return false;

	HRESULT hr = connPoints->FindConnectionPoint(__uuidof(IVsTextManagerEvents), connPt);
	return SUCCEEDED(hr) && *connPt;
}

bool CConnect::GetTextViewEventsPlug(IConnectionPoint** connPt, IVsTextView* view)
{
	CComQIPtr<IConnectionPointContainer> connPoints = view;
	if(!connPoints)
		return false;

	HRESULT hr = connPoints->FindConnectionPoint(__uuidof(IVsTextViewEvents), connPt);
	return SUCCEEDED(hr) && *connPt;
}

void CConnect::RegisterCommand(EnvDTE80::Commands2* cmdInterface, const wchar_t* name, const wchar_t* caption, const wchar_t* descr, CmdTriggerFunc handler)
{
	CComBSTR nameStr(name);
	CComBSTR captionStr(caption);
	CComBSTR descStr(descr);
	CComPtr<EnvDTE::Command> cmd;
	long status = EnvDTE::vsCommandStatusSupported | EnvDTE::vsCommandStatusEnabled;
	EnvDTE80::vsCommandStyle uiStyle = EnvDTE80::vsCommandStylePictAndText;
	EnvDTE80::vsCommandControlType uiType = EnvDTE80::vsCommandControlTypeButton;
	cmdInterface->AddNamedCommand2(g_addInInstance, nameStr, captionStr, descStr, VARIANT_TRUE, CComVariant(-1), 0, status, uiStyle, uiType, &cmd);

	std::wstring fullName = L"MetalScroll.Connect.";
	fullName += name;
	m_commandHandlers[fullName] = handler;
}

bool CConnect::FindRockScroll()
{
	CComPtr<EnvDTE::AddIns> addins;
	HRESULT hr = g_dte->get_AddIns(&addins);
	if(FAILED(hr))
		return false;

	long numAddins = 0;
	hr = addins->get_Count(&numAddins);
	if(FAILED(hr))
		return false;

	for(int i = 1; i <= numAddins; ++i)
	{
		CComPtr<EnvDTE::AddIn> addin;
		hr = addins->Item(CComVariant(i), &addin);
		if(FAILED(hr))
			continue;

		CComBSTR name;
		hr = addin->get_Name(&name);
		if(FAILED(hr))
			continue;

		if(_wcsicmp(name, L"rockscroll") != 0)
			continue;

		VARIANT_BOOL isLoaded = VARIANT_TRUE;
		addin->get_Connected(&isLoaded);
		// We usually show these message boxes when VS is loading, and there's a slight chance that they will end up under the splash screen.
		// In that case people will complain that MetalScroll hangs VS on their system because they won't see that they have to dismiss
		// a dialog box for it to continue loading. Therefore, we've made the message boxes system modal. It's ugly, but I can't think of
		// another solution.
		if(isLoaded)
		{
			MessageBoxA(0, "We have detected that RockScroll is loaded.\nPlease remove it if you wish to use MetalScroll.", "MetalScroll", MB_ICONERROR|MB_SYSTEMMODAL);
			return true;
		}
		else
		{
			Log("MetalScroll: Rockscroll is installed but not loaded.\n");
			//MessageBoxA(0, "We have detected that RockScroll is installed but not yet loaded.\nMetalScroll will load now, but if you also load RockScroll, it will probably cause Visual Studio to crash.", "MetalScroll", MB_ICONERROR|MB_SYSTEMMODAL);
			return false;
		}
	}

	return false;
}

STDMETHODIMP CConnect::OnConnection(IDispatch* application, ext_ConnectMode /*connectMode*/, IDispatch* addInInst, SAFEARRAY** /*custom*/)
{
	Log("MetalScroll: OnConnection()\n");

	HRESULT hr = application->QueryInterface(__uuidof(EnvDTE80::DTE2), (void**)&g_dte);
	if(FAILED(hr))
		return hr;

	if(FindRockScroll())
	{
		g_dte.Release();
		return E_FAIL;
	}

	hr = addInInst->QueryInterface(__uuidof(EnvDTE::AddIn), (void**)&g_addInInstance);
	if(FAILED(hr))
		return hr;

	CComQIPtr<IServiceProvider> sp = g_dte;
	if(!sp)
		return E_NOINTERFACE;

	hr = sp->QueryService(SID_SVsTextManager, IID_IVsTextManager, (void**)&g_textMgr);
	if(FAILED(hr) || !g_textMgr)
		return E_NOINTERFACE;

	CComPtr<IConnectionPoint> connPt;
	if(!GetTextManagerEventsPlug(&connPt))
		return E_NOINTERFACE;

	hr = connPt->Advise((IVsTextManagerEvents*)this, &m_textMgrEventsCookie);
	if(FAILED(hr))
		return hr;

	hr = g_textMgr->GetRegisteredMarkerTypeID(&g_markerTypeGUID, &g_highlightMarkerType);
	if(FAILED(hr))
	{
		Log("MetalScroll: No highlight marker.\n");
		g_highlightMarkerType = 0;
	}

	CComPtr<EnvDTE::Window> mainWnd;
	hr = g_dte->get_MainWindow(&mainWnd);
	if(FAILED(hr) || !mainWnd)
	{
		Log("MetalScroll: Can't get main VS window.\n");
		return E_NOINTERFACE;
	}

	long mainWndHandle;
	hr = mainWnd->get_HWnd(&mainWndHandle);
	if(FAILED(hr) || !mainWndHandle)
	{
		Log("MetalScroll: Can't get main VS window handle.\n");
		return E_NOINTERFACE;
	}

	g_mainVSHwnd = (HWND)mainWndHandle;

	MetalBar::Init();

	// Register the toggle command.
	CComQIPtr<EnvDTE::Commands> commands;
	g_dte->get_Commands(&commands);
	CComQIPtr<EnvDTE80::Commands2> commands2 = commands;

	RegisterCommand(commands2, L"Toggle", L"Toggle MetalScroll", L"Enables or disables the MetalScroll overview bar", &CConnect::OnToggle);

	HookAllScrollbars();

	Log("MetalScroll: OnConnection() done.\n");
	return S_OK;
}

STDMETHODIMP CConnect::OnDisconnection(ext_DisconnectMode /*removeMode*/, SAFEARRAY** /*custom*/)
{
	CComPtr<IConnectionPoint> textMgrEventsPlug;
	if(m_textMgrEventsCookie && GetTextManagerEventsPlug(&textMgrEventsPlug))
	{
		textMgrEventsPlug->Unadvise(m_textMgrEventsCookie);
		m_textMgrEventsCookie = 0;
	}

	MetalBar::Uninit();

	// Give up the global pointers.
	g_textMgr = 0;
	g_addInInstance = 0;
	g_dte = 0;
	return S_OK;
}

void STDMETHODCALLTYPE CConnect::OnRegisterView(IVsTextView* view)
{
	Log("MetalScroll: New view registered: 0x%p.\n", view);

	// Unfortunately, the window hasn't been created at this point yet, so we can't get the HWND
	// here. Register an even handler to catch SetFocus(), and get the HWND from there. We'll remove
	// the handler after the first SetFocus() as we don't care about getting more events once we
	// have the HWND.
	CComPtr<IConnectionPoint> connPt;
	if(!GetTextViewEventsPlug(&connPt, view))
		return;
	DWORD cookie;
	HRESULT hr = connPt->Advise((IVsTextViewEvents*)this, &cookie);
	if(FAILED(hr))
		return;

	Log("MetalScroll: Subscribed to view events.\n");
}

static BOOL CALLBACK FindScrollbars(HWND hwnd, LPARAM param)
{
	MetalBar::ScrollbarHandles* handles = (MetalBar::ScrollbarHandles*)param;

	char className[64];
	GetClassNameA(hwnd, className, _countof(className));
	if(strcmp(className, "ScrollBar") == 0)
	{
		LONG styles = GetWindowLong(hwnd, GWL_STYLE);
		if(styles & SBS_VERT)
			handles->vert = hwnd;
		else
			handles->horiz = hwnd;

		return TRUE;
	}

	// ReSharper adds a thing over the VS scrollbar.
	GetWindowTextA(hwnd, className, _countof(className));
	if(strcmp(className, "Error Stripe") == 0)
	{
		handles->resharper = hwnd;
		return TRUE;
	}

	return TRUE;
}

void CConnect::HookScrollbar(IVsTextView* view)
{
	MetalBar::ScrollbarHandles handles = { 0 };

	handles.editor = view->GetWindowHandle();
	if(!handles.editor)
		return;

	EnumChildWindows(GetParent(handles.editor), FindScrollbars, (LPARAM)&handles);
	if(!handles.vert)
	{
		Log("MetalScroll: Vertical scrollbar not found.\n");
		return;
	}

	if(GetWindowLongPtr(handles.vert, GWL_USERDATA) != 0)
	{
		Log("MetalScroll: Scrollbar has non-null user data.\n");
		return;
	}

	MetalBar* bar = new MetalBar(handles, view);
	Log("MetalScroll: Hooked view 0x%p. Scrollbars: %x, %x. Editor: %x. ReSharper: %x. MetalBar: 0x%p.\n", view, handles.vert, handles.horiz, handles.editor, handles.resharper, bar);
}

void STDMETHODCALLTYPE CConnect::OnSetFocus(IVsTextView* view)
{
	Log("MetalScroll: OnSetFocus(0x%p).\n", view);

	HookScrollbar(view);

	// Remove ourselves from the event list. Since we can't store the cookie we got
	// when we registered the event handler, we'll have to scan the event list looking
	// for our pointer.
	CComPtr<IConnectionPoint> connPt;
	if(!GetTextViewEventsPlug(&connPt, view))
		return;

	CComPtr<IEnumConnections> enumerator;
	HRESULT hr = connPt->EnumConnections(&enumerator);
	if(FAILED(hr) || !enumerator)
		return;

	CONNECTDATA connData;
	ULONG numRet;
	bool found = false;
	while( !found && SUCCEEDED(enumerator->Next(1, &connData, &numRet)) && (numRet > 0) )
	{
		if(connData.pUnk == (IVsTextViewEvents*)this)
		{
			connPt->Unadvise(connData.dwCookie);
			found = true;
		}
		connData.pUnk->Release();
	}
}

STDMETHODIMP CConnect::QueryStatus(BSTR /*bstrCmdName*/, EnvDTE::vsCommandStatusTextWanted NeededText, EnvDTE::vsCommandStatus* pStatusOption, VARIANT* /*pvarCommandText*/)
{
	if(NeededText != EnvDTE::vsCommandStatusTextWantedNone)
		return S_OK;

	*pStatusOption = (EnvDTE::vsCommandStatus)(EnvDTE::vsCommandStatusSupported | EnvDTE::vsCommandStatusEnabled);
	return S_OK;
}

STDMETHODIMP CConnect::Exec(BSTR bstrCmdName, EnvDTE::vsCommandExecOption ExecuteOption, VARIANT* /*pvarVariantIn*/, VARIANT* /*pvarVariantOut*/, VARIANT_BOOL* pvbHandled)
{
	*pvbHandled = VARIANT_FALSE;
	if(ExecuteOption != EnvDTE::vsCommandExecOptionDoDefault)
		return S_OK;

	CmdHandlerMapIt it = m_commandHandlers.find(bstrCmdName);
	if(it == m_commandHandlers.end())
		return S_OK;

	CmdTriggerFunc trigger = it->second;
	(this->*trigger)();
	*pvbHandled = VARIANT_TRUE;
	return S_OK;
}

void CConnect::HookAllScrollbars()
{
	CComPtr<IVsUIShellOpenDocument> shellOpenDoc;
	CComQIPtr<IServiceProvider> sp = g_dte;
	HRESULT hr = sp->QueryService(SID_SVsUIShellOpenDocument, IID_IVsUIShellOpenDocument, (void**)&shellOpenDoc);
	if(FAILED(hr))
		return;

	CComPtr<EnvDTE::Documents> docs;
	g_dte->get_Documents(&docs);

	long numDocs = 0;
	hr = docs->get_Count(&numDocs);
	if(FAILED(hr))
		return;

	for(int i = 1; i <= numDocs; ++i)
	{
		CComPtr<EnvDTE::Document> doc;
		HRESULT hr = docs->Item(CComVariant(i), &doc);
		if(FAILED(hr))
			continue;

		CComBSTR fullName;
		doc->get_FullName(&fullName);

		CComPtr<IVsUIHierarchy> hierarchy;
		VSITEMID itemId;
		CComPtr<IVsWindowFrame> wndFrame;
		BOOL isOpen;
		shellOpenDoc->IsDocumentOpen(0, 0, fullName, LOGVIEWID_TextView, 0, &hierarchy, &itemId, &wndFrame, &isOpen);
		if(!isOpen)
			continue;

		CComPtr<IVsCodeWindow> codeWnd;
		hr = wndFrame->QueryViewInterface(IID_IVsCodeWindow, (void**)&codeWnd);
		if(FAILED(hr))
			continue;

		IVsTextView* view = 0;
		hr = codeWnd->GetPrimaryView(&view);
		if(SUCCEEDED(hr) && view)
		{
			HookScrollbar(view);
			view->Release();
		}

		view = 0;
		hr = codeWnd->GetSecondaryView(&view);
		if(SUCCEEDED(hr) && view)
		{
			HookScrollbar(view);
			view->Release();
		}
	}
}

void CConnect::OnToggle()
{
	unsigned int enable = !MetalBar::IsEnabled();
	MetalBar::SetBarsEnabled(enable);
}
