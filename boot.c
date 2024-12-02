/*
 * uefi-ntfs: UEFI → NTFS/exFAT chain loader
 * Copyright © 2014-2024 Pete Batard <pete@akeo.ie>
 * With parts from rEFInd © 2012-2016 Roderick W. Smith
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "boot.h"

/* The following is autogenerated by the build process */
#include "version.h"

/* Global handle for the current executable */
static EFI_HANDLE MainImageHandle = NULL;

/* Strings used to identify the plaform */
#if defined(_M_X64) || defined(__x86_64__)
  static CHAR16* Arch = L"x64";
  static CHAR16* ArchName = L"64-bit x86";
#elif defined(_M_IX86) || defined(__i386__)
  static CHAR16* Arch = L"ia32";
  static CHAR16* ArchName = L"32-bit x86";
#elif defined (_M_ARM64) || defined(__aarch64__)
  static CHAR16* Arch = L"aa64";
  static CHAR16* ArchName = L"64-bit ARM";
#elif defined (_M_ARM) || defined(__arm__)
  static CHAR16* Arch = L"arm";
  static CHAR16* ArchName = L"32-bit ARM";
#elif defined(_M_RISCV64) || (defined (__riscv) && (__riscv_xlen == 64))
  static CHAR16* Arch = L"riscv64";
  static CHAR16* ArchName = L"64-bit RISC-V";
#elif  defined(_M_LOONGARCH64) || defined(__loongarch64)
static CHAR16* Arch = L"loongarch64";
static CHAR16* ArchName = L"64-bit LoongArch";
#else
#  error Unsupported architecture
#endif

/* Get the driver name from a driver handle */
static CHAR16* GetDriverName(CONST EFI_HANDLE DriverHandle)
{
	CHAR16 *DriverName;
	EFI_COMPONENT_NAME_PROTOCOL *ComponentName;
	EFI_COMPONENT_NAME2_PROTOCOL *ComponentName2;

	// Try EFI_COMPONENT_NAME2 protocol first
	if ( (gBS->OpenProtocol(DriverHandle, &gEfiComponentName2ProtocolGuid, (VOID**)&ComponentName2,
			MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL) == EFI_SUCCESS) &&
		 (ComponentName2->GetDriverName(ComponentName2, ComponentName2->SupportedLanguages, &DriverName) == EFI_SUCCESS))
		return DriverName;

	// Fallback to EFI_COMPONENT_NAME if that didn't work
	if ( (gBS->OpenProtocol(DriverHandle, &gEfiComponentNameProtocolGuid, (VOID**)&ComponentName,
			MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL) == EFI_SUCCESS) &&
		 (ComponentName->GetDriverName(ComponentName, ComponentName->SupportedLanguages, &DriverName) == EFI_SUCCESS))
		return DriverName;

	return L"(unknown driver)";
}

/*
 * Some UEFI firmwares (like HPQ EFI from HP notebooks) have DiskIo protocols
 * opened BY_DRIVER (by Partition driver in HP's case) even when no file system
 * is produced from this DiskIo. This then blocks our FS driver from connecting
 * and producing file systems.
 * To fix it we disconnect drivers that connected to DiskIo BY_DRIVER if this
 * is a partition volume and if those drivers did not produce file system.
 *
 * This code was originally derived from similar BSD-3-Clause licensed one
 * (a.k.a. Modified BSD License, which can be used in GPLv2+ works), found at:
 * https://sourceforge.net/p/cloverefiboot/code/3294/tree/rEFIt_UEFI/refit/main.c#l1271
 */
static VOID DisconnectBlockingDrivers(VOID) {
	EFI_STATUS Status;
	UINTN HandleCount = 0, Index, OpenInfoIndex, OpenInfoCount;
	EFI_HANDLE *Handles = NULL;
	CHAR16 *DevicePathString;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;
	EFI_BLOCK_IO_PROTOCOL *BlockIo;
	EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *OpenInfo;

	// Get all DiskIo handles
	Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiDiskIoProtocolGuid, NULL, &HandleCount, &Handles);
	if (EFI_ERROR(Status) || (HandleCount == 0))
		return;

	// Check every DiskIo handle
	for (Index = 0; Index < HandleCount; Index++) {
		// If this is not partition - skip it.
		// This is then whole disk and DiskIo
		// should be opened here BY_DRIVER by Partition driver
		// to produce partition volumes.
		Status = gBS->OpenProtocol(Handles[Index], &gEfiBlockIoProtocolGuid, (VOID**)&BlockIo,
			MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

		if (EFI_ERROR(Status))
			continue;
		if ((BlockIo->Media == NULL) || (!BlockIo->Media->LogicalPartition))
			continue;

		// If SimpleFileSystem is already produced - skip it, this is ok
		Status = gBS->OpenProtocol(Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Volume,
			MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (Status == EFI_SUCCESS)
			continue;

		DevicePathString = DevicePathToString(DevicePathFromHandle(Handles[Index]));

		// If no SimpleFileSystem on this handle but DiskIo is opened BY_DRIVER
		// then disconnect this connection
		Status = gBS->OpenProtocolInformation(Handles[Index], &gEfiDiskIoProtocolGuid, &OpenInfo, &OpenInfoCount);
		if (EFI_ERROR(Status)) {
			PrintWarning(L"  Could not get DiskIo protocol for %s: %r", DevicePathString, Status);
			FreePool(DevicePathString);
			continue;
		}

		for (OpenInfoIndex = 0; OpenInfoIndex < OpenInfoCount; OpenInfoIndex++) {
			if ((OpenInfo[OpenInfoIndex].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) == EFI_OPEN_PROTOCOL_BY_DRIVER) {
				Status = gBS->DisconnectController(Handles[Index], OpenInfo[OpenInfoIndex].AgentHandle, NULL);
				if (EFI_ERROR(Status)) {
					PrintError(L"  Could not disconnect '%s' on %s",
						GetDriverName(OpenInfo[OpenInfoIndex].AgentHandle), DevicePathString);
				} else {
					PrintWarning(L"  Disconnected '%s' on %s ",
						GetDriverName(OpenInfo[OpenInfoIndex].AgentHandle), DevicePathString);
				}
			}
		}
		FreePool(DevicePathString);
		FreePool(OpenInfo);
	}
	FreePool(Handles);
}

/*
 * Unload an existing file system driver.
 */
EFI_STATUS UnloadDriver(
	CONST EFI_HANDLE FileSystemHandle
)
{
	EFI_STATUS Status;
	UINTN OpenInfoCount, i;
	EFI_OPEN_PROTOCOL_INFORMATION_ENTRY* OpenInfo;
	EFI_DRIVER_BINDING_PROTOCOL* DriverBinding;
	CHAR16* DriverName;

	// Open the disk instance associated with the filesystem handle
	Status = gBS->OpenProtocolInformation(FileSystemHandle, &gEfiDiskIoProtocolGuid, &OpenInfo, &OpenInfoCount);
	if (EFI_ERROR(Status))
		return EFI_NOT_FOUND;

	// There may be multiple disk instances, including "phantom" ones (without a
	// bound driver) so try to process them all until we manage to unload a driver.
	for (i = 0; i < OpenInfoCount; i++) {
		// Obtain the info of the driver servicing this specific disk instance
		Status = gBS->OpenProtocol(OpenInfo[i].AgentHandle, &gEfiDriverBindingProtocolGuid,
			(VOID**)&DriverBinding, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(Status))
			continue;

		// Display the driver name and version, then unload it using its image handle
		DriverName = GetDriverName(OpenInfo[i].AgentHandle);
		PrintWarning(L"Unloading existing '%s v0x%x'", DriverName, DriverBinding->Version);
		Status = gBS->UnloadImage(DriverBinding->ImageHandle);
		if (EFI_ERROR(Status)) {
			PrintWarning(L"  Could not unload driver: %r", Status);
			continue;
		}
		return EFI_SUCCESS;
	}

	return EFI_NOT_FOUND;
}

/*
 * Display a centered application banner
 */
static VOID DisplayBanner(VOID)
{
	UINTN i, Len;
	CHAR16 String[BANNER_LINE_SIZE + 1];

	// The platform logo may still be displayed → remove it
	gST->ConOut->ClearScreen(gST->ConOut);

	SetText(TEXT_REVERSED);
	Print(L"\n%c", BOXDRAW_DOWN_RIGHT);
	for (i = 0; i < BANNER_LINE_SIZE - 2; i++)
		Print(L"%c", BOXDRAW_HORIZONTAL);
	Print(L"%c\n", BOXDRAW_DOWN_LEFT);

	UnicodeSPrint(String, ARRAY_SIZE(String), L"UEFI:NTFS %s (%s)", VERSION_STRING, Arch);
	Len = SafeStrLen(String);
	V_ASSERT(Len < BANNER_LINE_SIZE);
	Print(L"%c", BOXDRAW_VERTICAL);
	for (i = 1; i < (BANNER_LINE_SIZE - Len) / 2; i++)
		Print(L" ");
	Print(String);
	for (i += Len; i < BANNER_LINE_SIZE - 1; i++)
		Print(L" ");
	Print(L"%c\n", BOXDRAW_VERTICAL);

	UnicodeSPrint(String, ARRAY_SIZE(String), L"<https://un.akeo.ie>");
	Len = SafeStrLen(String);
	V_ASSERT(Len < BANNER_LINE_SIZE);
	Print(L"%c", BOXDRAW_VERTICAL);
	for (i = 1; i < (BANNER_LINE_SIZE - Len) / 2; i++)
		Print(L" ");
	Print(String);
	for (i += Len; i < BANNER_LINE_SIZE - 1; i++)
		Print(L" ");
	Print(L"%c\n", BOXDRAW_VERTICAL);

	Print(L"%c", BOXDRAW_UP_RIGHT);
	for (i = 0; i < 77; i++)
		Print(L"%c", BOXDRAW_HORIZONTAL);
	Print(L"%c\n\n", BOXDRAW_UP_LEFT);
	DefText();
}

/*
 * Application entry-point
 * NB: This must be set to 'efi_main' for gnu-efi crt0 compatibility
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE BaseImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	CONST CHAR8 FsMagic[2][8] = {
		{ 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '} ,
		{ 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '} };
	CONST CHAR16* FsName[] = { L"NTFS", L"exFAT" };
	CONST CHAR16* DriverName[] = { L"ntfs", L"exfat" };
	CHAR16 DriverPath[64], LoaderPath[64];
	CHAR16* DevicePathString;
	EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
	EFI_STATUS Status;
	EFI_DEVICE_PATH *DevicePath = NULL, *ParentDevicePath = NULL, *BootDiskPath = NULL;
	EFI_DEVICE_PATH *BootPartitionPath = NULL;
	EFI_HANDLE* Handles = NULL, ImageHandle, DriverHandleList[2] = { 0 };
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Volume;
	EFI_FILE_SYSTEM_VOLUME_LABEL* VolumeInfo;
	EFI_FILE_HANDLE Root;
	EFI_BLOCK_IO_PROTOCOL *BlockIo;
	// We'll search for "bootmgr.dll" in UEFI bootloaders to identify Windows
	// bootloaders, but we don't want to match our own bootloader in the process.
	// So we use a modifiable string buffer where the first character is not set.
	CHAR8 BootMgrName[] = "_ootmgr.dll", BootMgrNameFirstLetter = 'b';
	CHAR8* Buffer;
	INTN SecureBootStatus;
	UINTN Index, FsType = 0, Try, Event, HandleCount = 0, Size;
	BOOLEAN SameDevice, WindowsBootMgr = FALSE;

#if defined(_GNU_EFI)
	InitializeLib(BaseImageHandle, SystemTable);
#endif
	MainImageHandle = BaseImageHandle;

	DisplayBanner();
	PrintSystemInfo();
	SecureBootStatus = GetSecureBootStatus();
	SetText(TEXT_WHITE);
	Print(L"[INFO]");
	DefText();
	Print(L" Secure Boot status: ");
	if (SecureBootStatus == 0) {
		Print(L"Disabled\n");
	} else {
		SetText((SecureBootStatus > 0) ? TEXT_WHITE : TEXT_YELLOW);
		Print(L"%s\n", (SecureBootStatus > 0) ? L"Enabled" : L"Setup");
		DefText();
	}

	Status = gBS->OpenProtocol(MainImageHandle, &gEfiLoadedImageProtocolGuid,
		(VOID**)&LoadedImage, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status)) {
		PrintError(L"Unable to access boot image interface");
		goto out;
	}

	PrintInfo(L"Disconnecting potentially blocking drivers");
	DisconnectBlockingDrivers();

	// Identify our boot partition and disk
	BootPartitionPath = DevicePathFromHandle(LoadedImage->DeviceHandle);
	BootDiskPath = GetParentDevice(BootPartitionPath);

	PrintInfo(L"Searching for target partition on boot disk:");
	DevicePathString = DevicePathToString(BootDiskPath);
	PrintInfo(L"  %s", DevicePathString);
	SafeFree(DevicePathString);
	// Enumerate all disk handles
	Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiDiskIoProtocolGuid,
		NULL, &HandleCount, &Handles);
	if (EFI_ERROR(Status)) {
		PrintError(L"  Failed to list disks");
		goto out;
	}

	// Go through the partitions and find the one that has the USB Disk we booted from
	// as parent and that isn't the FAT32 boot partition
	for (Index = 0; Index < HandleCount; Index++) {
		// Note: The Device Path obtained from DevicePathFromHandle() should NOT be freed!
		DevicePath = DevicePathFromHandle(Handles[Index]);
		// Eliminate the partition we booted from
		if (CompareDevicePaths(DevicePath, BootPartitionPath) == 0)
			continue;
		// Ensure that we look for the NTFS/exFAT partition on the same device.
		ParentDevicePath = GetParentDevice(DevicePath);
		SameDevice = (CompareDevicePaths(BootDiskPath, ParentDevicePath) == 0);
		SafeFree(ParentDevicePath);
		// The check breaks QEMU testing (since we can't easily emulate
		// a multipart device on the fly) so only do it for release.
#if !defined(_DEBUG)
		if (!SameDevice)
			continue;
#else
		(VOID)SameDevice;	// Silence a MinGW warning
#endif
		// Read the first block of the partition and look for the FS magic in the OEM ID
		Status = gBS->OpenProtocol(Handles[Index], &gEfiBlockIoProtocolGuid,
			(VOID**)&BlockIo, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(Status))
			continue;
		Buffer = (CHAR8*)AllocatePool(BlockIo->Media->BlockSize);
		if (Buffer == NULL)
			continue;
		Status = BlockIo->ReadBlocks(BlockIo, BlockIo->Media->MediaId, 0, BlockIo->Media->BlockSize, Buffer);
		for (FsType = 0; (FsType < ARRAY_SIZE(FsName)) && 
			(CompareMem(&Buffer[3], FsMagic[FsType], sizeof(FsMagic[FsType])) != 0); FsType++);
		FreePool(Buffer);
		if (EFI_ERROR(Status))
			continue;
		if (FsType < ARRAY_SIZE(FsName))
			break;
	}

	if (Index >= HandleCount) {
		Status = EFI_NOT_FOUND;
		PrintError(L"  Could not locate target partition");
		goto out;
	}
	PrintInfo(L"Found %s target partition:", FsName[FsType]);
	DevicePathString = DevicePathToString(DevicePath);
	PrintInfo(L"  %s", DevicePathString);
	SafeFree(DevicePathString);

	// Test for presence of file system protocol (to see if there already is
	// a filesystem driver servicing this partition)
	Status = gBS->OpenProtocol(Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
		(VOID**)&Volume, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_TEST_PROTOCOL);

	// Only handle partitions that are flagged as serviced or needing service
	if (Status != EFI_SUCCESS && Status != EFI_UNSUPPORTED) {
		PrintError(L"Could not check for %s service", FsName[FsType]);
		goto out;
	}

	// Because of the AMI NTFS driver bug (https://github.com/pbatard/AmiNtfsBug) as
	// well as reports of issues when using an NTFS driver different from ours, we
	// unconditionally try to unload any native file system driver that is servicing
	// our target partition.
	if (Status == EFI_SUCCESS) {
		// Unload the driver and, if successful, flag the partition as needing service
		if (UnloadDriver(Handles[Index]) == EFI_SUCCESS)
			Status = EFI_UNSUPPORTED;
	}

	// If the partition is not/no-longer serviced, start our file system driver.
	if (Status == EFI_UNSUPPORTED) {
		PrintInfo(L"Starting %s driver service:", FsName[FsType]);

		// Use 'rufus' in the driver path, so that we don't accidentally latch onto a user driver
		UnicodeSPrint(DriverPath, ARRAY_SIZE(DriverPath), L"\\efi\\rufus\\%s_%s.efi", DriverName[FsType], Arch);
		DevicePath = FileDevicePath(LoadedImage->DeviceHandle, DriverPath);
		if (DevicePath == NULL) {
			Status = EFI_DEVICE_ERROR;
			PrintError(L"  Unable to set path for '%s'", DriverPath);
			goto out;
		}

		// Attempt to load the driver.
		// NB: If running in a Secure Boot enabled environment, LoadImage() will fail if
		// the image being loaded does not pass the Secure Boot signature validation.
		Status = gBS->LoadImage(FALSE, MainImageHandle, DevicePath, NULL, 0, &ImageHandle);
		SafeFree(DevicePath);
		if (EFI_ERROR(Status)) {
			// Some platforms (e.g. Intel NUCs) return EFI_ACCESS_DENIED for Secure Boot
			// validation errors. Return a much more explicit EFI_SECURITY_VIOLATION then.
			if ((Status == EFI_ACCESS_DENIED) && (SecureBootStatus >= 1))
				Status = EFI_SECURITY_VIOLATION;
			PrintError(L"  Unable to load driver '%s'", DriverPath);
			goto out;
		}

		// NB: Some HP firmwares refuse to start drivers that are not of type 'EFI Boot
		// System Driver'. For instance, a driver of type 'EFI Runtime Driver' produces
		// a 'Load Error' on StartImage() with these firmwares => check the type.
		Status = gBS->OpenProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
			(VOID**)&LoadedImage, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(Status)) {
			PrintError(L"  Unable to access driver interface");
			goto out;
		}
		if (LoadedImage->ImageCodeType != EfiBootServicesCode) {
			Status = EFI_LOAD_ERROR;
			PrintError(L"  '%s' is not a Boot System Driver", DriverPath);
			goto out;
		}

		// Load was a success - attempt to start the driver
		Status = gBS->StartImage(ImageHandle, NULL, NULL);
		if (EFI_ERROR(Status)) {
			PrintError(L"  Unable to start driver");
			goto out;
		}
		PrintInfo(L"  %s", GetDriverName(ImageHandle));

		// Calling ConnectController() on a handle, with a NULL-terminated list of
		// drivers will start all the drivers from the list that can service it
		DriverHandleList[0] = ImageHandle;
		DriverHandleList[1] = NULL;
		Status = gBS->ConnectController(Handles[Index], DriverHandleList, NULL, TRUE);
		if (EFI_ERROR(Status)) {
			PrintError(L"  Could not start %s partition service", FsName[FsType]);
			goto out;
		}
	}

	// Our target file system is case sensitive, so we need to figure out the
	// case sensitive version of the following
	UnicodeSPrint(LoaderPath, ARRAY_SIZE(LoaderPath), L"\\efi\\boot\\boot%s.efi", Arch);

	PrintInfo(L"Opening target %s partition:", FsName[FsType]);
	// Open the the volume, with retry, as we may need to wait before poking
	// at the FS content, in case the system is slow to start our service...
	for (Try = 0; ; Try++) {
		Status = gBS->OpenProtocol(Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
			(VOID**)&Volume, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
		if (!EFI_ERROR(Status))
			break;
		PrintError(L"  Could not open partition");
		if (Try >= NUM_RETRIES)
			goto out;
		PrintWarning(L"  Waiting %d seconds before retrying...", DELAY);
		gBS->Stall(DELAY * 1000000);
	}

	// Open the root directory
	Root = NULL;
	Status = Volume->OpenVolume(Volume, &Root);
	if ((EFI_ERROR(Status)) || (Root == NULL)) {
		PrintError(L"  Could not open Root directory");
		goto out;
	}

	// Get the volume label while we're at it
	Size = FILE_INFO_SIZE;
	VolumeInfo = (EFI_FILE_SYSTEM_VOLUME_LABEL*)AllocateZeroPool(Size);
	if (VolumeInfo != NULL) {
		Status = Root->GetInfo(Root, &gEfiFileSystemVolumeLabelInfoIdGuid, &Size, VolumeInfo);
		// Some UEFI firmwares return EFI_BUFFER_TOO_SMALL, even with
		// a large enough buffer, unless the exact size is requested.
		if ((Status == EFI_BUFFER_TOO_SMALL) && (Size <= FILE_INFO_SIZE))
			Status = Root->GetInfo(Root, &gEfiFileSystemVolumeLabelInfoIdGuid, &Size, VolumeInfo);
		if (Status == EFI_SUCCESS)
			PrintInfo(L"  Volume label is '%s'", VolumeInfo->VolumeLabel);
		else
			PrintWarning(L"  Could not read volume label: [%d] %r\n", (Status & 0x7FFFFFFF), Status);
		FreePool(VolumeInfo);
	}

	PrintInfo(L"This system uses %s UEFI => searching for %s EFI bootloader", ArchName, Arch);
	// This next call corrects the casing to the required one
	Status = SetPathCase(Root, LoaderPath);
	if (EFI_ERROR(Status)) {
		PrintError(L"  Could not locate '%s'", &LoaderPath[1]);
		goto out;
	}

	// At this stage, our DevicePath is the partition we are after
	PrintInfo(L"Launching '%s'...", &LoaderPath[1]);

	// Now attempt to chain load boot###.efi on the target partition
	DevicePath = FileDevicePath(Handles[Index], LoaderPath);
	if (DevicePath == NULL) {
		Status = EFI_DEVICE_ERROR;
		PrintError(L"  Could not create path");
		goto out;
	}
	Status = gBS->LoadImage(FALSE, MainImageHandle, DevicePath, NULL, 0, &ImageHandle);
	SafeFree(DevicePath);
	if (EFI_ERROR(Status)) {
		if ((Status == EFI_ACCESS_DENIED) && (SecureBootStatus >= 1))
			Status = EFI_SECURITY_VIOLATION;
		PrintError(L"  Load failure");
		goto out;
	}

	// Look for a "bootmgr.dll" string in the loaded image to identify a Windows bootloader.
	BootMgrName[0] = BootMgrNameFirstLetter;
	Status = gBS->OpenProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
		(VOID**)&LoadedImage, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status)) {
		PrintWarning(L"  Unable to inspect loaded executable");
	} else for (Index = 0x40; Index < LoadedImage->ImageSize - sizeof(BootMgrName); Index++) {
		if (CompareMem((CHAR8*)((UINTN)LoadedImage->ImageBase + Index),
			BootMgrName, sizeof(BootMgrName)) == 0) {
			WindowsBootMgr = TRUE;
			PrintInfo(L"Starting Microsoft Windows bootmgr...");
			break;
		}
	}

	Status = gBS->StartImage(ImageHandle, NULL, NULL);
	if (EFI_ERROR(Status)) {
		// Windows bootmgr simply returns EFI_NO_MAPPING on any internal error or security
		// violation, instead of halting and explicitly reporting the issue, leaving many
		// users extremely confused as to why their media did not boot. This can happen, for
		// instance, if the machine has had the BlackLotus UEFI lock enabled and the user
		// attempts to boot a pre 2023.05 version of the Windows installers.
		// We therefore take it upon ourselves to report what Windows bootmgr will not report.
		if (Status == EFI_NO_MAPPING && WindowsBootMgr) {
			SetText(TEXT_RED); Print(L"[FAIL]"); DefText();
			Print(L"   Windows bootmgr encountered a security validation or internal error\n");
		} else
			PrintError(L"  Start failure");
	}

out:
	SafeFree(ParentDevicePath);
	SafeFree(BootDiskPath);
	SafeFree(Handles);

	// Wait for a keystroke on error
	if (EFI_ERROR(Status)) {
		SetText(TEXT_YELLOW);
		Print(L"\nPress any key to exit.\n");
		DefText();
		gST->ConIn->Reset(gST->ConIn, FALSE);
		gST->BootServices->WaitForEvent(1, &gST->ConIn->WaitForKey, &Event);
	}

	return Status;
}
