#include <windows.h> 
#include <tchar.h>
#include <stdio.h>
#include <strsafe.h>
#include <conio.h>

////////////////////
/// Constants
////////////////////
#define BUFSIZE 4096

#define CCOM_WRITE_TEXT		0x2
#define WRF_FAILED	0x00
#define WRF_WRITE_END	0x01
#define WRF_SRCDS_END	0x02

////////////////////
/// Handles
////////////////////
HANDLE g_hChildStd_OUT_Rd = NULL;
HANDLE g_hChildStd_OUT_Wr = NULL;

PROCESS_INFORMATION piProcInfo;
HANDLE map_file_;
HANDLE event_parent_send_;
HANDLE event_child_send_;

////////////////////
/// Srcds Control
////////////////////
LPVOID GetMappedBuffer() {
	return (LPVOID)MapViewOfFile(map_file_, FILE_MAP_READ | FILE_MAP_WRITE, NULL, NULL, 0);
}

void ReleaseMappedBuffer(LPVOID pBuffer) {
	UnmapViewOfFile(pBuffer);
}

int WaitForResponse() {
	HANDLE waitForEvents[2];
	waitForEvents[0] = event_child_send_;
	waitForEvents[1] = piProcInfo.hProcess;

	DWORD waitResult = WaitForMultipleObjects(2, waitForEvents, false, INFINITE);
	if (waitResult == WAIT_OBJECT_0)
		return WRF_WRITE_END;
	else if (waitResult == (WAIT_OBJECT_0 + 1))
		return WRF_SRCDS_END;
	else
		return WRF_FAILED;
}

bool WasRequestSuccessful() {
	int *pBuf = (int *)GetMappedBuffer();
	bool success = pBuf[0] == 1;
	ReleaseMappedBuffer(pBuf);
	return success;
}

bool WriteText(const char* input) {
	int* pBuf = (int*)GetMappedBuffer();
	pBuf[0] = CCOM_WRITE_TEXT;

	strncpy_s((char*)(pBuf + 1), BUFSIZE - sizeof(int), input, strlen(input) + 1);

	ReleaseMappedBuffer(pBuf);
	SetEvent(event_parent_send_);

	if (WaitForResponse() != WRF_WRITE_END) {
		return false;
	}

	return WasRequestSuccessful();
}

void InitSrcdsControl(void) {
	SECURITY_ATTRIBUTES secAttrb;
	secAttrb.nLength = sizeof(SECURITY_ATTRIBUTES);
	secAttrb.lpSecurityDescriptor = NULL;
	secAttrb.bInheritHandle = true;

	map_file_ = CreateFileMapping(INVALID_HANDLE_VALUE, &secAttrb, PAGE_READWRITE, 0, 65536, NULL);
	if (map_file_ == NULL)
		printf("Could not create file mapping object");

	event_parent_send_ = CreateEvent(&secAttrb, false, false, NULL);
	if (event_parent_send_ == NULL)
		printf("Failed to create parent send event");

	event_child_send_ = CreateEvent(&secAttrb, false, false, NULL);
	if (event_child_send_ == NULL)
		printf("Failed to create child send event");
}

void DeinitSrcdsControl(void) {
	if (map_file_ != INVALID_HANDLE_VALUE) {
		CloseHandle(map_file_);
		map_file_ = INVALID_HANDLE_VALUE;
	}
	if (event_parent_send_ != INVALID_HANDLE_VALUE)	{
		CloseHandle(event_parent_send_);
		event_parent_send_ = INVALID_HANDLE_VALUE;
	}
	if (event_child_send_ != INVALID_HANDLE_VALUE) {
		CloseHandle(event_child_send_);
		event_child_send_ = INVALID_HANDLE_VALUE;
	}
}

void StartSrcds(const TCHAR* szWorkingDir, const TCHAR* szParams) {
	TCHAR szCmdline[512];
	wsprintf(szCmdline, L"%s\\srcds.exe -HFILE %d -HPARENT %d -HCHILD %d %s", szWorkingDir, map_file_, event_parent_send_, event_child_send_, szParams);

	STARTUPINFO siStartInfo;

	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = g_hChildStd_OUT_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	siStartInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	// Create the child process. 
	BOOL bSuccess = CreateProcess(NULL,
		szCmdline,     // command line 
		NULL,          // process security attributes 
		NULL,          // primary thread security attributes 
		TRUE,          // handles are inherited 
		CREATE_NO_WINDOW,             // creation flags 
		NULL,          // use parent's environment 
		szWorkingDir,          // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION
}
////////////////////
/// Threads
////////////////////
DWORD WINAPI ReadThread(void* data) {
	DWORD dwRead, dwWritten;
	CHAR chBuf[BUFSIZE];
	BOOL bSuccess = FALSE;
	HANDLE hParentStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	while(1) {
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		bSuccess = WriteFile(hParentStdOut, chBuf,
			dwRead, &dwWritten, NULL);
		if (!bSuccess) break;
	}

	return 0;
}

DWORD WINAPI WriteThread(void* data) {
	CHAR chBuf[BUFSIZE];

	while (1) {
		fgets(chBuf, BUFSIZE, stdin);
		printf("-> Executing Command: ");
		WriteText(chBuf);
	}

	return 0;
}

////////////////////
/// main
////////////////////
int _tmain(int argc, TCHAR* argv[]) {
	SECURITY_ATTRIBUTES saAttr;

	printf("\n->Start of Srcds execution.\n");

	// Set the bInheritHandle flag so pipe handles are inherited. 
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	// Create a pipe for the child process's STDOUT. 
	if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
		printf("StdoutRd CreatePipe\n");

	// Ensure the read handle to the pipe for STDOUT is not inherited.
	if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
		printf("Stdout SetHandleInformation\n");

	InitSrcdsControl();

	TCHAR szCurrentDir[260];
	GetCurrentDirectory(260, szCurrentDir);
	TCHAR szParams[BUFSIZE] = TEXT(" -console -game left4dead2");
	if (argc > 1) {
		ZeroMemory(szParams, BUFSIZE);
		for (int i = 1; i < argc; i++) {
			wsprintf(szParams, TEXT("%s %s"), szParams, argv[i]);
		}
	}

	StartSrcds(szCurrentDir, szParams);

	HANDLE threadRead = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
	HANDLE threadWrite = CreateThread(NULL, 0, WriteThread, NULL, 0, NULL);
	
	WaitForSingleObject(piProcInfo.hProcess, INFINITE);

	TerminateThread(threadRead, 0);
	TerminateThread(threadWrite, 0);

	CloseHandle(piProcInfo.hProcess);
	CloseHandle(piProcInfo.hThread);
	
	DeinitSrcdsControl();
	printf("\n->End of Srcds execution.\n");

	system("pause");
    return 0;
}

