#include "application.h"

#include <windows.h>

int APIENTRY wWinMain(
	_In_ HINSTANCE instance,
	_In_opt_ HINSTANCE previousInstance,
	_In_ LPWSTR commandLine,
	_In_ int showCommand) {
	UNREFERENCED_PARAMETER(previousInstance);
	UNREFERENCED_PARAMETER(commandLine);
	UNREFERENCED_PARAMETER(showCommand);

	return crg::RunApplication(instance);
}
