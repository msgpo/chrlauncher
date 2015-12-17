// chrlauncher
// Copyright (c) 2015 Henry++

#include <windows.h>
#include <regex>

#include "main.h"
#include "app_helper.h"
#include "routine.h"

#include "resource.h"

#include "bzlib.h"
#include "mar.h"
#include "unzip.h"

#define BLOCKSIZE 16384

CApp app (APP_NAME, APP_NAME_SHORT, APP_VERSION, APP_COPYRIGHT);

#define CHROMIUM_UPDATE_URL L"http://chromium.woolyss.com/api/?os=windows&bit=%d&out=xml"
#define FIREFOX_UPDATE_URL L"https://aus3.mozilla.org/update/1/Firefox/%s/default/WINNT_%s-msvc/%s/%s/update.xml?force=1"

DWORD architecture = 0;

CString browser_name;
CString browser_name_full;
CString browser_directory;
CString browser_exe;
CString browser_args;
CString browser_version;

VOID _Application_SetPercent (DWORD v, DWORD t)
{
	INT percent = 0;

	if (t)
	{
		percent = (double (v) / double (t)) * 100.0;
	}

	SendDlgItemMessage (app.GetHWND (), IDC_PROGRESS, PBM_SETPOS, percent, 0);

	_r_status_settext (app.GetHWND (), IDC_STATUSBAR, 1, _r_fmt (L"%s/%s", _r_fmt_size64 (v), _r_fmt_size64 (t)));
}

VOID _Application_SetStatus (LPCWSTR text)
{
	_r_status_settext (app.GetHWND (), IDC_STATUSBAR, 0, text);
}

CString xml_parse (std::wstring s, LPCWSTR v, BOOL is_tag)
{
	std::wsmatch m;
	std::wregex e;

	if (is_tag)
	{
		e.assign (_r_fmt (L"<%s>(.*?)<\\/%s>", v, v).GetString ()); // tag extractor
	}
	else
	{
		e.assign (_r_fmt (L" %s=[\"']?((?:.(?![\"']?\\s+(?:\\S+)=|[>\"']))+.)[\"']?", v).GetString ()); // attribute extractor
	}

	return std::regex_search (s, m, e) ? m[1].str ().c_str () : L"";
}

DWORD total_mar = 0;
DWORD total_read_mar = 0;

static int mar_callback (MarFile* mar, const MarItem* item, LPVOID)
{
	CHAR inbuf[BLOCKSIZE] = {0};
	CHAR outbuf[BLOCKSIZE] = {0};

	CString name = browser_directory + L"\\" + CA2W (item->name, CP_UTF8);

	int pos = name.ReverseFind (L'/');

	if (pos != -1)
	{
		SHCreateDirectoryEx (nullptr, _r_normalize_path (name.Mid (0, pos)), nullptr);
	}

	bz_stream strm = {0};

	if (BZ2_bzDecompressInit (&strm, 0, 0) == BZ_OK)
	{
		HANDLE f = CreateFile (name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		if (f != INVALID_HANDLE_VALUE)
		{
			DWORD offset = 0, inlen, outlen, ret;

			while (1)
			{
				if (!item->length) { break; }

				if (offset < (int)item->length && strm.avail_in == 0)
				{
					inlen = mar_read (mar, item, offset, inbuf, BLOCKSIZE);

					if (inlen <= 0) { break; }

					total_read_mar += inlen;
					offset += inlen;
					strm.next_in = inbuf;
					strm.avail_in = inlen;
				}

				strm.next_out = outbuf;
				strm.avail_out = BLOCKSIZE;

				ret = BZ2_bzDecompress (&strm);

				if (ret != BZ_OK && ret != BZ_STREAM_END) { break; }

				outlen = BLOCKSIZE - strm.avail_out;

				if (outlen)
				{
					DWORD written = 0;

					if (!WriteFile (f, outbuf, outlen, &written, nullptr) || !written) { break; }
				}

				if (ret == BZ_STREAM_END) { break; }

				_Application_SetPercent (total_read_mar, total_mar);
			}

			CloseHandle (f);
		}

		BZ2_bzDecompressEnd (&strm);
	}

	return 0;
}

BOOL _Browser_InstallMar (LPCWSTR path)
{
	BOOL result = FALSE;

	MarFile* mar = mar_wopen (path);
	MarItem* item = nullptr;

	if (mar)
	{
		for (INT i = 0; i < TABLESIZE; ++i)
		{
			item = mar->item_table[i];

			while (item) { total_mar += item->length; item = item->next; }
		}

		if (!mar_enum_items (mar, &mar_callback, nullptr))
		{
			result = TRUE;
		}

		mar_close (mar);
	}

	return result;
}

BOOL _Browser_InstallZip (LPCWSTR path)
{
	BOOL result = FALSE;

	ZIPENTRY ze = {0};

	HZIP hz = OpenZip (path, nullptr);

	if (IsZipHandleU (hz))
	{
		DWORD total_size = 0;
		DWORD total_progress = 0;
		DWORD count_all = 0; // this is our progress so far

		// count total files
		GetZipItem (hz, -1, &ze);
		INT total_files = ze.index;

		// check archive is right package
		GetZipItem (hz, 0, &ze);

		if (wcsncmp (ze.name, L"chrome-win32", 12) == 0)
		{
			// count size of unpacked files
			for (INT i = 0; i < total_files; i++)
			{
				GetZipItem (hz, i, &ze);

				total_size += ze.unc_size;
			}

			for (INT i = 1; i < total_files; i++)
			{
				GetZipItem (hz, i, &ze);

				CString name = ze.name;
				name = name.Mid (name.Find (L"/"));

				CString file = _r_fmt (L"%s\\%s", browser_directory, name);

				if ((ze.attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					SHCreateDirectoryEx (nullptr, file, nullptr);
				}
				else
				{
					HANDLE f = CreateFile (file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

					if (f != INVALID_HANDLE_VALUE)
					{
						CHAR* buff = new char[BLOCKSIZE];
						DWORD count_file = 0;

						for (ZRESULT zr = ZR_MORE; zr == ZR_MORE;)
						{
							zr = UnzipItem (hz, i, buff, BLOCKSIZE);

							ULONG bufsize = BLOCKSIZE;

							if (zr == ZR_OK)
							{
								bufsize = ze.unc_size - count_file;
							}

							DWORD written = 0;
							WriteFile (f, buff, bufsize, &written, nullptr);

							count_file += bufsize;
							count_all += bufsize;

							_Application_SetPercent (count_all, total_size);
						}

						delete[] buff;

						CloseHandle (f);
					}
				}
			}

			result = TRUE;
		}

		CloseZip (hz);
	}

	return result;
}

BOOL _Browser_DownloadUpdate (CString url)
{
	BOOL result = FALSE;

	HINTERNET internet = nullptr;
	HINTERNET connect = nullptr;

	// get path
	WCHAR path[MAX_PATH] = {0};

	GetTempPath (MAX_PATH, path);
	GetTempFileName (path, nullptr, 0, path);

	// download file
	_Application_SetStatus (I18N (&app, IDS_STATUS_DOWNLOAD, 0));
	_Application_SetPercent (0, 0);

	internet = InternetOpen (app.GetUserAgent (), INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);

	if (internet)
	{
		connect = InternetOpenUrl (internet, url, nullptr, 0, INTERNET_FLAG_RESYNCHRONIZE | INTERNET_FLAG_NO_COOKIES, 0);

		if (connect)
		{
			DWORD status = 0, size = sizeof (status);
			HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE, &status, &size, nullptr);

			if (status == HTTP_STATUS_OK)
			{
				DWORD total_size = 0;

				size = sizeof (total_size);

				HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_CONTENT_LENGTH, &total_size, &size, nullptr);

				DWORD out = 0;
				DWORD total_written = 0;

				CHAR* buff = new CHAR[BLOCKSIZE];

				HANDLE f = CreateFile (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

				if (f != INVALID_HANDLE_VALUE)
				{
					while (1)
					{
						if (!InternetReadFile (connect, buff, BLOCKSIZE, &out) || !out)
						{
							break;
						}

						DWORD written = 0;
						WriteFile (f, buff, out, &written, nullptr);

						total_written += out;

						_Application_SetPercent (total_written, total_size);
					}

					CloseHandle (f);
				}

				delete[] buff;

				// installing update
				_Application_SetStatus (I18N (&app, IDS_STATUS_INSTALL, 0));
				_Application_SetPercent (0, 0);

				// create directory
				SHCreateDirectoryEx (nullptr, browser_directory, nullptr);

				// extract archive
				if (browser_name.CompareNoCase (L"firefox") == 0) { result = _Browser_InstallMar (path); }
				else if (browser_name.CompareNoCase (L"chromium") == 0) { result = _Browser_InstallZip (path); }

				if (result)
				{
					app.ConfigSet (L"BrowserCheckDate", DWORD (_r_unixtime_now ()));
				}

				DeleteFile (path);
			}
		}
	}

	return result;
}

UINT WINAPI _Browser_CheckUpdate (LPVOID)
{
	HINTERNET internet = nullptr, connect = nullptr;

	CStringMap result;

	DWORD days = app.ConfigGet (L"BrowserCheckPeriod", 1);
	BOOL is_exists = _r_file_is_exists (browser_exe);
	BOOL is_success = FALSE;

	if (days || !is_exists)
	{
		if (!is_exists || _r_unixtime_now () - app.ConfigGet (L"BrowserCheckDate", 0) >= (86400 * days))
		{
			internet = InternetOpen (app.GetUserAgent (), INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);

			if (internet)
			{
				CString url;

				if (browser_name.CompareNoCase (L"firefox") == 0)
				{
					url.Format (FIREFOX_UPDATE_URL, browser_version.IsEmpty () ? L"10.0" : browser_version, architecture == 32 ? L"x86" : L"x86_64", app.ConfigGet (L"FirefoxLocalization", L"en-US"), app.ConfigGet (L"FirefoxChannel", L"release"));
				}
				else
				{
					url.Format (CHROMIUM_UPDATE_URL, architecture);
				}

				connect = InternetOpenUrl (internet, url, nullptr, 0, INTERNET_FLAG_RESYNCHRONIZE | INTERNET_FLAG_NO_COOKIES, 0);

				if (connect)
				{
					DWORD dwStatus = 0, dwStatusSize = sizeof (dwStatus);
					HttpQueryInfo (connect, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE, &dwStatus, &dwStatusSize, nullptr);

					if (dwStatus == HTTP_STATUS_OK)
					{
						DWORD out = 0;
						BOOL b = 0;

						CStringA buffera, bufferafull;
						CStringW bufferw;

						do
						{
							b = InternetReadFile (connect, buffera.GetBuffer (BLOCKSIZE), BLOCKSIZE, &out);
							buffera.ReleaseBuffer ();

							if (out)
							{
								bufferafull.Append (buffera.Mid (0, out));
							}
						}
						while (b && out);

						if (!bufferafull.IsEmpty ())
						{
							bufferw = CA2W (bufferafull.Trim (" \r\n"), CP_UTF8);

							if (browser_name.CompareNoCase (L"firefox") == 0)
							{
								result[L"version"] = xml_parse (bufferw.GetString (), L"appVersion", FALSE);
								result[L"url"] = xml_parse (bufferw.GetString (), L"URL", FALSE);
								result[L"date"] = xml_parse (bufferw.GetString (), L"buildID", FALSE);
							}
							else
							{
								result[L"version"] = xml_parse (bufferw.GetString (), L"version", TRUE);
								result[L"url"] = xml_parse (bufferw.GetString (), L"url", TRUE);
								result[L"date"] = _r_fmt_date (wcstoull (xml_parse (bufferw.GetString (), L"timestamp", TRUE), nullptr, 10), FDTF_SHORTDATE | FDTF_SHORTTIME);

								if (result[L"url"].IsEmpty ())
								{
									result[L"url"].Format (L"https://storage.googleapis.com/chromium-browser-continuous/%s/%s/chrome-win32.zip", architecture == 32 ? L"Win" : L"Win_x64", xml_parse (bufferw.GetString (), L"revision", TRUE));
								}
							}

							is_success = TRUE;
						}
					}
				}
			}

			InternetCloseHandle (connect);
			InternetCloseHandle (internet);
		}

		if (is_success)
		{
			SetDlgItemText (app.GetHWND (), IDC_BROWSER, _r_fmt (I18N (&app, IDS_BROWSER, 0), browser_name_full, architecture));
			SetDlgItemText (app.GetHWND (), IDC_CURRENTVERSION, _r_fmt (I18N (&app, IDS_CURRENTVERSION, 0), browser_version.IsEmpty () ? L"<not found>" : browser_version));
			SetDlgItemText (app.GetHWND (), IDC_VERSION, _r_fmt (I18N (&app, IDS_VERSION, 0), result[L"version"]));
			SetDlgItemText (app.GetHWND (), IDC_DATE, _r_fmt (I18N (&app, IDS_DATE, 0), result[L"date"]));

			// check for new version
			if (!is_exists || wcsncmp (browser_version, result[L"version"], result[L"version"].GetLength ()) != 0)
			{
				if (result[L"url"].IsEmpty ())
				{
					_r_msg (app.GetHWND (), MB_OK | MB_ICONERROR, APP_NAME, I18N (&app, IDS_STATUS_NOTFOUND, 0), browser_name_full, architecture);
				}
				else
				{
					_r_windowtoggle (app.GetHWND (), TRUE);

					_Browser_DownloadUpdate (result[L"url"]);
				}
			}
		}
		else
		{
			app.ConfigSet (L"BrowserCheckDate", DWORD (_r_unixtime_now ()));
		}
	}

	PostMessage (app.GetHWND (), WM_DESTROY, 0, 0);

	return ERROR_SUCCESS;
}

CString _Browser_GetVersion ()
{
	CString result;

	DWORD verHandle = 0;
	DWORD verSize = GetFileVersionInfoSize (browser_exe, &verHandle);

	if (verSize)
	{
		LPSTR verData = new char[verSize];

		if (GetFileVersionInfo (browser_exe, verHandle, verSize, verData))
		{
			LPBYTE buffer = nullptr;
			UINT size = 0;

			if (VerQueryValue (verData, L"\\", (VOID FAR* FAR*)&buffer, &size))
			{
				if (size)
				{
					VS_FIXEDFILEINFO *verInfo = (VS_FIXEDFILEINFO*)buffer;

					if (verInfo->dwSignature == 0xfeef04bd)
					{
						// Doesn't matter if you are on 32 bit or 64 bit,
						// DWORD is always 32 bits, so first two revision numbers
						// come from dwFileVersionMS, last two come from dwFileVersionLS

						result.Format (L"%d.%d.%d.%d", (verInfo->dwFileVersionMS >> 16) & 0xffff, (verInfo->dwFileVersionMS >> 0) & 0xffff, (verInfo->dwFileVersionLS >> 16) & 0xffff, (verInfo->dwFileVersionLS >> 0) & 0xffff);
					}
				}
			}
		}

		delete[] verData;
	}

	return result;
}

BOOL _Browser_Run ()
{
	return _r_run (nullptr, _r_fmt (L"\"%s\" %s", browser_exe, browser_args).GetBuffer (), browser_directory);
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			app.SetHWND (hwnd);

			INT parts[] = {app.GetDPI (230), -1};
			SendDlgItemMessage (hwnd, IDC_STATUSBAR, SB_SETPARTS, 2, (LPARAM)parts);

			// select browser by arguments
			INT numargs = 0;
			LPWSTR* arga = CommandLineToArgvW (GetCommandLine (), &numargs);
			CString args;

			for (INT i = 1; i < numargs; i++)
			{
				if (wcsicmp (arga[i], L"/browser") == 0 && i < numargs)
				{
					browser_name = arga[i + 1];
				}
			}

			LocalFree (arga);

			// select browser by config (otherwise)
			if (browser_name.IsEmpty ())
			{
				browser_name = app.ConfigGet (L"BrowserName", L"chromium");
			}

			// configure paths
			if (browser_name.CompareNoCase (L"firefox") == 0)
			{
				browser_name_full = L"Mozilla Firefox";
				browser_directory = _r_normalize_path (app.ConfigGet (L"FirefoxDirectory", L".\\firefox\\bin"));
				browser_exe = browser_directory + L"\\firefox.exe";
				browser_args = app.ConfigGet (L"FirefoxCommandLine", L"-profile \"..\\profile\" -no-remote");

				SendMessage (hwnd, WM_SETICON, ICON_SMALL, (LPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (101), GetSystemMetrics (SM_CXSMICON)));
				SendMessage (hwnd, WM_SETICON, ICON_BIG, (LPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (101), GetSystemMetrics (SM_CXICON)));

			}
			else
			{
				browser_name_full = L"Chromium";
				browser_directory = _r_normalize_path (app.ConfigGet (L"ChromiumDirectory", L".\\chromium\\bin"));
				browser_exe = browser_directory + L"\\chrome.exe";
				browser_args = app.ConfigGet (L"ChromiumCommandLine", L"--user-data-dir=..\\profile --no-default-browser-check");
			}

			// get architecture...
			architecture = app.ConfigGet (L"BrowserArchitecture", 0);

			if (!architecture || (architecture != 64 && architecture != 32))
			{
				architecture = 0;

				// on XP only 32-bit supported
				if (_r_system_validversion (5, 1, VER_EQUAL))
				{
					architecture = 32;
				}

				// ...by executable
				DWORD exe_type = 0;

				if (GetBinaryType (browser_exe, &exe_type))
				{
					if (exe_type == SCS_32BIT_BINARY)
					{
						architecture = 32;
					}
					else if (exe_type == SCS_64BIT_BINARY)
					{
						architecture = 64;
					}
				}

				if (!architecture)
				{
					// ...by processor architecture
					SYSTEM_INFO si = {0};

					GetNativeSystemInfo (&si);

					if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) { architecture = 32; }
					else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) { architecture = 64; }
				}
			}

			// get current version
			if (browser_name.CompareNoCase (L"firefox") == 0)
			{
				GetPrivateProfileString (L"App", L"Version", nullptr, browser_version.GetBuffer (100), 100, _r_fmt (L"%s\\application.ini", browser_directory));

				browser_version.ReleaseBuffer ();
			}
			else
			{
				browser_version = _Browser_GetVersion ();
			}

			// start update checking
			_beginthreadex (nullptr, 0, &_Browser_CheckUpdate, nullptr, 0, nullptr);

			break;
		}

		case WM_CLOSE:
		{
			if (_r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, I18N (&app, IDS_QUESTION_BUSY, 0)) != IDYES)
			{
				return TRUE;
			}

			DestroyWindow (hwnd);
			break;
		}

		case WM_DESTROY:
		{
			_Browser_Run ();
			PostQuitMessage (0);

			break;
		}

		case WM_QUERYENDSESSION:
		{
			SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
			return TRUE;
		}

		case WM_LBUTTONDOWN:
		{
			SendMessage (hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, NULL);
			break;
		}

		case WM_ENTERSIZEMOVE:
		case WM_EXITSIZEMOVE:
		case WM_CAPTURECHANGED:
		{
			LONG_PTR exstyle = GetWindowLongPtr (hwnd, GWL_EXSTYLE);

			if ((exstyle & WS_EX_LAYERED) != WS_EX_LAYERED)
			{
				SetWindowLongPtr (hwnd, GWL_EXSTYLE, exstyle | WS_EX_LAYERED);
			}

			SetLayeredWindowAttributes (hwnd, 0, (msg == WM_ENTERSIZEMOVE) ? 100 : 255, LWA_ALPHA);
			SetCursor (LoadCursor (nullptr, (msg == WM_ENTERSIZEMOVE) ? IDC_SIZEALL : IDC_ARROW));

			break;
		}

		case WM_NOTIFY:
		{
			switch (LPNMHDR (lparam)->code)
			{
				case NM_CLICK:
				case NM_RETURN:
				{
					ShellExecute (nullptr, nullptr, PNMLINK (lparam)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			switch (LOWORD (wparam))
			{
				case IDCANCEL: // process Esc key
				case IDM_EXIT:
				{
					SendMessage (hwnd, WM_CLOSE, 0, 0);
					break;
				}

				case IDM_WEBSITE:
				{
					ShellExecute (hwnd, nullptr, _APP_WEBSITE_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_ABOUT:
				{
					app.CreateAboutWindow ();
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

INT APIENTRY wWinMain (HINSTANCE, HINSTANCE, LPWSTR, INT)
{
	if (app.CreateMainWindow (&DlgProc))
	{
		MSG msg = {0};

		while (GetMessage (&msg, nullptr, 0, 0))
		{
			if (!IsDialogMessage (app.GetHWND (), &msg))
			{
				TranslateMessage (&msg);
				DispatchMessage (&msg);
			}
		}
	}

	return ERROR_SUCCESS;
}
