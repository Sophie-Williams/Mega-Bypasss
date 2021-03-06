// MainConsole.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <Windows.h>
#include <string>
#include "ManualMap.h"
#include "Include.h"
#include "ABClient.h"
#include "Util.h"
#include <Windows.h>
#include <vector>
#include "Utilities.h"
#include <cstdio>
#include "drv_image.hpp"
#include "util.hpp"
#include "capcom.hpp"
#include "structs.hpp"
#include "loader.hpp"
#include "capcomsys.hpp"
#include <cassert>
#include <tlhelp32.h>
#include <Psapi.h>
#include <map>
#include "Error.h"
#include "MemoryController.h"
#include "SimpleMapper.h"
#include "LockedMemory.h"
#include "DriverCom.h"

#pragma comment(lib, "capcom.lib")
#pragma comment(lib, "psapi.lib")

PVOID AllocateKernelMemory(CapcomContext* CpCtx, KernelContext* KrCtx, SIZE_T Size)
{
	NON_PAGED_DATA static auto k_ExAllocatePool = KrCtx->GetProcAddress<fnFreeCall>("ExAllocatePool");
	NON_PAGED_DATA static uint64_t MemOut;

	CpCtx->ExecuteInKernel(NON_PAGED_LAMBDA(PVOID Pv)
	{
		MemOut = Khk_CallPassive(k_ExAllocatePool, 0ull, Pv);
	}, (PVOID)Size);

	return (PVOID)MemOut;
}

BOOL ExposeKernelMemoryToProcess(MemoryController& Mc, PVOID Memory, SIZE_T Size, uint64_t EProcess)
{
	Mc.AttachTo(EProcess);

	BOOL Success = FALSE;

	Mc.IterPhysRegion(Memory, Size, [&](PVOID Va, uint64_t Pa, SIZE_T Sz)
	{
		auto Info = Mc.QueryPageTableInfo(Va);

		Info.Pml4e->user = TRUE;
		Info.Pdpte->user = TRUE;
		Info.Pde->user = TRUE;

		if (!Info.Pde || (Info.Pte && (!Info.Pte->present)))
		{
			Success = FALSE;
		}
		else
		{
			if (Info.Pte)
				Info.Pte->user = TRUE;
		}
	});

	Mc.Detach();

	return Success;
}

PUCHAR FindKernelPadSinglePage(PUCHAR Start, SIZE_T Size)
{
	PUCHAR It = Start;

	MEMORY_BASIC_INFORMATION Mbi;

	PUCHAR StreakStart = 0;
	int Streak = 0;

	do
	{
		if ((0x1000 - (uint64_t(It) & 0xFFF)) < Size)
		{
			It++;
			continue;
		}

		if (*It == 0)
		{
			if (!Streak)
				StreakStart = It;
			Streak++;
		}
		else
		{
			Streak = 0;
			StreakStart = 0;
		}

		if (Streak >= Size)
			return StreakStart;

		VirtualQuery(It, &Mbi, sizeof(Mbi));

		It++;
	} while ((Mbi.Protect == PAGE_EXECUTE_READWRITE || Mbi.Protect == PAGE_EXECUTE_READ || Mbi.Protect == PAGE_EXECUTE_WRITECOPY));
	return 0;
}

uint32_t FindProcess(const std::string& Name)
{
	PROCESSENTRY32 ProcessEntry;
	ProcessEntry.dwSize = sizeof(PROCESSENTRY32);
	HANDLE ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (Process32First(ProcessSnapshot, &ProcessEntry))
	{
		do
		{
			if (!stricmp(ProcessEntry.szExeFile, Name.data()))
			{
				CloseHandle(ProcessSnapshot);
				return ProcessEntry.th32ProcessID;
			}
		} while (Process32Next(ProcessSnapshot, &ProcessEntry));
	}
	CloseHandle(ProcessSnapshot);
	return 0;
}

// If you like to load any one of your drivers
#define srvName L"Capcom"
void installService(LPCWSTR path)
{
	SC_HANDLE handle = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	SC_HANDLE service = ::CreateService(
		handle,
		srvName,
		L"Capcom",
		GENERIC_READ | GENERIC_EXECUTE,
		SERVICE_WIN32_OWN_PROCESS,
		SERVICE_AUTO_START,
		SERVICE_ERROR_IGNORE,
		path,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	);
}

int LoadPrivilege(void)
{
	HANDLE hToken;
	LUID Value;
	TOKEN_PRIVILEGES tp;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return(GetLastError());
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Value))
		return(GetLastError());
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = Value;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
		return(GetLastError());
	CloseHandle(hToken);
	return 1;
}

typedef void(_cdecl* func)();


char* spoofmac()
{
	char buffer[60];
	unsigned long size = sizeof(buffer);
	HKEY software;
	LPCTSTR location;
	char adapternum[10] = "";
	char numbers[11] = "0123456789";
	char editlocation[] = "System\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002bE10318}\\0000";
	char macaddress[60];

	cout << "\n//////////////////////////////////////////////////////////////////\nPlease Enter number of Network Adapter to Spoof or type 'E' to Exit.\nE.g. 6\n\nNumber: ";
	cin >> adapternum;

	if (adapternum[0] == 'E')
	{

		exit(0);
	}

	if (strlen(adapternum) == 2)
	{
		editlocation[strlen(editlocation) - 2] = adapternum[0];
		editlocation[strlen(editlocation) - 1] = adapternum[1];
	}

	if (strlen(adapternum) == 1)
	{
		editlocation[strlen(editlocation) - 1] = adapternum[0];
	}
	if (strlen(adapternum) != 1 && strlen(adapternum) != 2)
	{
		cout << "Invalid Network Adapter Selected\n\n";
		exit(0);
	}

	cout << "Please enter the desired Spoofed Mac Address\nE.g. 02113A0D4D66\n\nNew Mac Address: ";
	cin >> macaddress;

	location = (LPCTSTR)editlocation;
	strcpy(buffer, macaddress);
	size = sizeof(buffer);
	RegCreateKey(HKEY_LOCAL_MACHINE, location, &software);
	RegSetValueEx(software, L"NetworkAddress", 0, REG_SZ, (LPBYTE)buffer, size);
	RegCloseKey(software);

}

void readregistry()
{

	char driver[60] = "";
	char mac[60] = "";
	char numbers[11] = "0123456789";
	char editlocation[] = "System\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002bE10318}\\0000";
	unsigned long driversize = sizeof(driver);
	unsigned long macsize = sizeof(mac);
	DWORD type;
	HKEY software;
	LPCTSTR location;
	int tenscount = 0;
	int onescount = 0;

	for (int x = 0; x <= 19; x += 1)
	{
		strcpy(driver, "");
		driversize = sizeof(driver);
		strcpy(mac, "");
		macsize = sizeof(mac);

		if (editlocation[strlen(editlocation) - 1] == '9')
		{
			tenscount += 1;
			onescount = 0;
			editlocation[strlen(editlocation) - 2] = numbers[tenscount];
		}

		editlocation[strlen(editlocation) - 1] = numbers[onescount];
		location = (LPCTSTR)editlocation;
		RegCreateKey(HKEY_LOCAL_MACHINE, location, &software);
		RegQueryValueEx(software, L"DriverDesc", 0, &type, (LPBYTE)driver, &driversize);
		RegQueryValueEx(software, L"NetworkAddress", 0, &type, (LPBYTE)mac, &macsize);
		RegCloseKey(software);
		onescount += 1;
	}

}


int main()
{
	using namespace std;
	string Input;
	string DriverDir = "BypassDriver.sys";
	DWORD GamePID;

	cout << "------------------------------" << endl;
	cout << "Welcome To Sagaan's Mega Bypasses" << endl;
	cout << "Thanks To: @Daax ( Kernel God ) @wlan (Manual Map Drivers ) @harakirinox ( help ) @asmjs ( No NoBastian ) & All Of UknownCheats.me" << endl;
	cout << "------------------------------" << endl;
	cout << "Bypasses: " << endl;
	cout << "1) Driver ( Cheat, HWID Spoofer, Test Mode Spoofer ) " << endl;
	cout << "2) Usermode HWID Spoofer " << endl;
	cout << "3) Lsass Bypass " << endl;
	cout << "4) Injector Bypass " << endl;
	cout << "Please Choose One Bypass: ";
	cin >> Input;
	cout << "------------------------------" << endl << endl;


	int Answer = stoi(Input.c_str());
	if (Answer == 1)
	{
		CopyFile(L"Capcom.sys", L"C:\\Windows\\Capcom.sys", TRUE);
		Sleep(300);

		bool capcomload = loader::load_vuln_driver((uint8_t*)capcom_sys, sizeof(capcom_sys), L"C:\\Windows\\Capcom.sys", L"Capcom");
		printf("[+] loaded capcom driver: %i\n", capcomload);

		const auto capcom = std::make_unique<capcom::capcom_driver>();

		const auto _get_module = [&capcom](std::string_view name)
		{
			return capcom->get_kernel_module(name);
		};

		const auto _get_export_name = [&capcom](uintptr_t base, const char* name)
		{
			return capcom->get_export(base, name);
		};

		const std::function<uintptr_t(uintptr_t, uint16_t)> _get_export_ordinal = [&capcom](uintptr_t base, uint16_t ord)
		{
			return capcom->get_export(base, ord);
		};
		sizeof(SYSTEM_INFORMATION_CLASS::SystemBasicInformation);
		std::vector<uint8_t> driver_image;
		drvmap::util::open_binary_file(DriverDir, driver_image);
		drvmap::drv_image driver(driver_image);

		const auto kernel_memory = capcom->allocate_pool(driver.size(), kernel::NonPagedPool, true);

		assert(kernel_memory != 0);

		printf("[+] allocated 0x%llX bytes at 0x%I64X\n", driver.size(), kernel_memory);

		driver.fix_imports(_get_module, _get_export_name, _get_export_ordinal);

		printf("[+] imports fixed\n");

		driver.map();

		printf("[+] sections mapped in memory\n");

		driver.relocate(kernel_memory);

		printf("[+] relocations fixed\n");

		const auto _RtlCopyMemory = capcom->get_system_routine<drvmap::structs::RtlCopyMemoryFn>(L"RtlCopyMemory");

		const auto size = driver.size();
		const auto source = driver.data();
		const auto entry_point = kernel_memory + driver.entry_point();

		capcom->run([&kernel_memory, &source, &size, &_RtlCopyMemory](auto get_mm)
		{
			_RtlCopyMemory((void*)kernel_memory, source, size);
		});

		printf("[+] calling entry point at 0x%I64X\n", entry_point);

		auto status = STATUS_SUCCESS;
		const auto capcom_base = capcom->get_kernel_module("Capcom");
		capcom->run([&entry_point, &status, &kernel_memory, &capcom_base](auto mm_get) {
			using namespace drvmap::structs;
			status = ((PDRIVER_INITIALIZE)entry_point)((_DRIVER_OBJECT*)kernel_memory, (PUNICODE_STRING)capcom_base);
		});

		if (NT_SUCCESS(status))
		{
			printf("[+] successfully created driver object!\n");

			const auto _RtlZeroMemory = capcom->get_system_routine<drvmap::structs::RtlZeroMemoryFn>(L"RtlZeroMemory");
			const auto header_size = driver.header_size();

			capcom->run([&_RtlZeroMemory, &kernel_memory, &header_size](auto mm_get) {
				_RtlZeroMemory((void*)kernel_memory, header_size);
			});

			printf("[+] wiped headers!\n");
		}
		else
		{
			printf("[-] creating of driver object failed! 0x%I32X\n", status);

		}

		capcom->close_driver_handle();
		capcomload = loader::unload_vuln_driver(L"C:\\Windows\\Capcom.sys", L"Capcom");
		printf("[+] unloaded capcom driver: %i\n", capcomload);
		cout << "Driver Option Chosen!" << endl;
		
		Sleep(3000);
		system("CLS");

		
		cout << "Driver Loaded!!" << endl;
		Wrappers Driver("\\\\.\\MegaBypassDriver");
		DWORD ProcessId;
		std::cout << "Please enter Game PID:" << std::endl;
		std::cin >> ProcessId;
		Driver.SetTargetPid(ProcessId); //Set PID in driver
		DWORD_PTR MainModule = Driver.GetMainModule();

		
	}
	else if (Answer == 2)
	{
		readregistry();
		spoofmac();
		cout << "Mac Spoofed! " << endl;
	}
	else if (Answer == 3)
	{
		using namespace asmjs;
		ABClient abc;

		if (LoadPrivilege())
		{
			cout << "Press Enter Once Done Injecting DLL Into Lsass" << endl;
			system("pause");

			abc.Connect();
			abc.Ping();

			string GameName;
			cout << "Game Name: ";
			cin >> GameName;
			abc.AccuireProcessHandle(Util::GetProcessIdsByName(GameName)[0], 0x1478);
			auto baseGame = abc.GetProcessModuleBase(GameName);
			cout << endl;
			cout << "Base Address: " << baseGame << endl;
			system("pause");
		}
		else
		{
			cout << "SetDebugPriv Failed..." << endl;
			system("pause");
		}
	}
	else if (Answer == 4)
	{
		std::string ProcessName;
		std::string DllPath;
		std::map<std::string, bool> Flags;

		SetConsoleTitleA("The Perfect Injector");
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 0xF);
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 0x8);

		if (!ProcessName.size())
		{
			printf("Enter the target process name: ");
			std::cin >> std::ws;
			getline(std::cin, ProcessName);
		}
		if (!DllPath.size())
		{
			printf("Enter the path to the module: ");
			std::cin >> std::ws;
			getline(std::cin, DllPath);
		}

		printf("\n");
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 13);


		printf("Flags:         ");


		printf("Dll Path:      '%s'\n", DllPath.data());
		printf("Process Name:  '%s'\n", ProcessName.data());
		printf("\n");

		// Initialize physical memory controller
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 12);
		KernelContext* KrCtx;
		CapcomContext* CpCtx;
		MemoryController Controller = Mc_InitContext(&CpCtx, &KrCtx);

		if (Controller.CreationStatus)
			ERROR("Controller Raised A Creation Status");

		// Hook a very commonly used function
		PUCHAR _TlsGetValue = (PUCHAR)GetProcAddress(GetModuleHandleA("KERNEL32"), "TlsGetValue"); // Not &TlsGetValue to avoid __imp intermodule calls

																								   // kernel32._TlsGetValue - EB 1E                 - jmp kernel32._TlsGetValue+
																								   // KERNEL32._TlsGetValue - E9 CBD70100           - jmp KERNEL32.UTUnRegister+160
		assert(*_TlsGetValue == 0xE9 || *_TlsGetValue == 0xEB);
		PUCHAR Target = (*_TlsGetValue == 0xEB) ? (_TlsGetValue + 2 + *(int8_t*)(_TlsGetValue + 1)) : (_TlsGetValue + 5 + *(int32_t*)(_TlsGetValue + 1));

		// Map module to kernel and create a hook stub
		std::vector<std::pair<PVOID, SIZE_T>> UsedRegions;

		TlsLockedHookController* TlsHookController = Mp_MapDllAndCreateHookEntry(DllPath, _TlsGetValue, Target, !Flags["noloadlib"], [&](SIZE_T Size)
		{
			//return VirtualAlloc( 0, Size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
			PVOID Memory = AllocateKernelMemory(CpCtx, KrCtx, Size);
			ExposeKernelMemoryToProcess(Controller, Memory, Size, Controller.CurrentEProcess);
			ZeroMemory(Memory, Size);
			UsedRegions.push_back({ Memory, Size });
			return Memory;
		});

		// Unload driver
		Cl_FreeContext(CpCtx);
		Kr_FreeContext(KrCtx);

		printf("\n");
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 10);

		if (Flags["waitkey"])
		{
			printf("Waiting for F2 key...\n");
			while (!(GetAsyncKeyState(VK_F2) & 0x8000)) Sleep(10);
		}

		printf("Waiting for %s...\n", ProcessName.data());

		uint64_t Pid = 0;
		while (!Pid)
		{
			Pid = FindProcess(ProcessName);
			Sleep(10);
		}

		printf("Found %s. Pid 0x%04x!\n", ProcessName.data(), Pid);

		printf("\n");
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 11);


		uint64_t EProcess = Controller.FindEProcess(Pid);
		printf("[-] EProcess:                               %16llx\n", EProcess);

		if (!EProcess)
			ERROR("EProcess Not Valid");

		// Expose region to process
		for (auto Region : UsedRegions)
		{
			printf("[-] Exposing %16llx (%08x bytes) to pid:%6llx\n", Region.first, Region.second, Pid);
			ExposeKernelMemoryToProcess(Controller, Region.first, Region.second, EProcess);
		}

		std::vector<BYTE> PidBasedHook =
		{
			0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00,        // mov rax, gs:[0x30]
			0x8B, 0x40, 0x40,                                            // mov eax,[rax+0x40] ; pid
			0x3D, 0xDD, 0xCC, 0xAB, 0x0A,                                // cmp eax, 0xAABCCDD
			0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,                          // jne 0xAABBCC
			0x48, 0xB8, 0xAA, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x00, 0x00,  // mov rax, 0xAABBCCDDEEAA
			0xFF, 0xE0                                                   // jmp rax
		};

		PUCHAR PadSpace = FindKernelPadSinglePage(_TlsGetValue, PidBasedHook.size());

		if (!PadSpace)
			ERROR("Couldn't Find Appropriate Padding");

		printf("[-] Hooking TlsGetValue @                   %16llx\n", _TlsGetValue);
		printf("[-] TlsGetValue Redirection Target:         %16llx\n", Target);
		printf("[-] Stub located at:                        %16llx\n", PadSpace);
		printf("[-] Image located at:                       %16llx\n", TlsHookController);

		*(uint32_t*)(&PidBasedHook[0xD]) = Pid; // Pid
		*(int32_t*)(&PidBasedHook[0x13]) = Target - (PadSpace + 0x17); // Jmp
		*(PUCHAR*)(&PidBasedHook[0x19]) = &TlsHookController->EntryBytes; // Hook target

																		  // Backup and complete hook
		BYTE Jmp[5];
		Jmp[0] = 0xE9;
		*(int32_t*)(Jmp + 1) = PadSpace - (_TlsGetValue + 5);

		std::vector<BYTE> Backup1(PidBasedHook.size(), 0);
		std::vector<BYTE> Backup2(5, 0);

		TlsHookController->NumThreadsWaiting = 0;
		TlsHookController->IsFree = FALSE;

		Controller.Detach();


		auto AssertCoW = [&](PVOID Page)
		{
			VirtualLock(Page, 0x1);

			PSAPI_WORKING_SET_EX_INFORMATION Ws;
			Ws.VirtualAddress = Page;
			QueryWorkingSetEx(HANDLE(-1), &Ws, sizeof(Ws));

			if (!Ws.VirtualAttributes.Shared)
				ERROR("Page Not CoW");

			VirtualUnlock(Page, 0x1);
		};

		// check maching memory checks AND is CoW check 

		printf("[-] Writing stub to padding...\n");
		AssertCoW(PadSpace);
		Controller.AttachIfCanRead(EProcess, PadSpace);
		Controller.ReadVirtual(PadSpace, Backup1.data(), PidBasedHook.size());
		Controller.WriteVirtual(PidBasedHook.data(), PadSpace, PidBasedHook.size());

		printf("[-] Writing the hook to TlsGetValue...\n");
		AssertCoW(_TlsGetValue);
		Controller.AttachIfCanRead(EProcess, _TlsGetValue);
		Controller.ReadVirtual(_TlsGetValue, Backup2.data(), 5);
		Controller.WriteVirtual(Jmp, _TlsGetValue, 5);

		printf("[-] Hooked! Waiting for threads to spin...\n");

		// Wait for threads to lock
		uint64_t TStart = GetTickCount64();
		while (!Controller.ReadVirtual<BYTE>(&TlsHookController->NumThreadsWaiting) && !(GetAsyncKeyState(VK_F1) & 0x8000) && ((GetTickCount64() - TStart) < 5000))
			Sleep(1);
		printf("[-] Threads spinning:                       %16llx\n", TlsHookController->NumThreadsWaiting);

		// Restore Backup

		Controller.AttachIfCanRead(EProcess, _TlsGetValue);
		Controller.WriteVirtual(Backup2.data(), _TlsGetValue, 5);


		if (TlsHookController->NumThreadsWaiting)
			printf("[-] Unhooked and started thread hijacking!\n");
		else
			printf("[-] ERROR: Wait timed out...\n");

		TlsHookController->IsFree = TRUE;
		Sleep(2000);

		Controller.AttachIfCanRead(EProcess, PadSpace);
		Controller.WriteVirtual(Backup1.data(), PadSpace, PidBasedHook.size());

		system("pause");
	}

	return 0;
}

