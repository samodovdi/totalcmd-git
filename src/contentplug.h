// Minimal subset of the Total Commander content plugin (WDX) interface.
#pragma once
#include <windows.h>

#define ft_nomorefields 0
#define ft_numeric_32   1
#define ft_boolean      6
#define ft_nosuchfield  -1

typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} ContentDefaultParamStruct;
