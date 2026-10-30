// Driver-side ABI declarations for the Windows ABI probe.
//
// Adopted without declaration changes from Khronos31/BonDriver_Userland
// 60651d3eaf232b129b0a94f1c4dea373a075bd9c include/bondriver/Abi.h.
// Only this provenance comment was added; vtable order, structure layout and
// scalar widths are the production contract.
//
// EDCB / TVTest compatible BonDriver ABI declarations.
//
// This header declares the public IBonDriver / IBonDriver2 interface and the
// EDCB STRUCT_IBONDRIVER layouts.  The vtable order, structure layout and
// 32-bit scalar widths must match what EDCB and LibISDB consumers expect; see
// README and NOTICE for the compatibility contract and provenance.
//
// On Windows the real Win32 scalar types (from <windows.h>) are used so a
// translation unit may include both this header and windows.h.  On other
// platforms equivalent fixed-width types are supplied.
#pragma once

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cstdint>
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int BOOL;
#  if defined(__WCHAR_MAX__) && (__WCHAR_MAX__ > 0xFFFF)
typedef uint16_t BON16CHAR;
#  else
typedef wchar_t BON16CHAR;
#  endif
#endif

#if defined(_WIN32)
typedef WCHAR BON16CHAR;
#endif

#ifndef TRUE
#  define TRUE 1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

#ifndef BONDRIVER_API
#  if defined(_WIN32)
#    if defined(BONDRIVER_USING_DEF)
// Exports are declared in a .def file so x86 names stay undecorated.
#      define BONDRIVER_API extern "C"
#    else
#      define BONDRIVER_API extern "C" __declspec(dllexport)
#    endif
#  else
#    define BONDRIVER_API extern "C" __attribute__((visibility("default")))
#  endif
#endif

// Interface classes keep default visibility so their RTTI/vtable are visible
// to consumers performing dynamic_cast across the shared library boundary.
#ifndef BONDRIVER_INTERFACE
#  if defined(_WIN32)
#    define BONDRIVER_INTERFACE
#  else
#    define BONDRIVER_INTERFACE __attribute__((visibility("default")))
#  endif
#endif

class BONDRIVER_INTERFACE IBonDriver
{
public:
	virtual const BOOL OpenTuner(void) = 0;
	virtual void CloseTuner(void) = 0;

	virtual const BOOL SetChannel(const BYTE bCh) = 0;
	virtual const float GetSignalLevel(void) = 0;

	virtual const DWORD WaitTsStream(const DWORD dwTimeOut = 0) = 0;
	virtual const DWORD GetReadyCount(void) = 0;

	virtual const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) = 0;
	virtual const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) = 0;

	virtual void PurgeTsStream(void) = 0;

	virtual void Release(void) = 0;
};

// Plain aggregate with the exact EDCB field order.  The library fills the
// function pointers; consumers only read them.
struct STRUCT_IBONDRIVER
{
	void *pCtx;
	const void *pEnd;
	BOOL (*pF00)(void *);
	void (*pF01)(void *);
	BOOL (*pF02)(void *, BYTE);
	float (*pF03)(void *);
	DWORD (*pF04)(void *, DWORD);
	DWORD (*pF05)(void *);
	BOOL (*pF06)(void *, BYTE *, DWORD *, DWORD *);
	BOOL (*pF07)(void *, BYTE **, DWORD *, DWORD *);
	void (*pF08)(void *);
	void (*pF09)(void *);
};

class BONDRIVER_INTERFACE IBonDriver2 : public IBonDriver
{
public:
	virtual const BON16CHAR* GetTunerName(void) = 0;

	virtual const BOOL IsTunerOpening(void) = 0;

	virtual const BON16CHAR* EnumTuningSpace(const DWORD dwSpace) = 0;
	virtual const BON16CHAR* EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const DWORD GetCurSpace(void) = 0;
	virtual const DWORD GetCurChannel(void) = 0;

// IBonDriver
	virtual void Release(void) = 0;
};

struct STRUCT_IBONDRIVER2
{
	STRUCT_IBONDRIVER st;
	const BON16CHAR* (*pF10)(void *);
	BOOL (*pF11)(void *);
	const BON16CHAR* (*pF12)(void *, DWORD);
	const BON16CHAR* (*pF13)(void *, DWORD, DWORD);
	BOOL (*pF14)(void *, DWORD, DWORD);
	DWORD (*pF15)(void *);
	DWORD (*pF16)(void *);
};

// Sentinel returned by GetCurSpace/GetCurChannel when no channel is current.
constexpr DWORD BONDRIVER_SPACE_INVALID = 0xFFFFFFFFu;
constexpr DWORD BONDRIVER_CHANNEL_INVALID = 0xFFFFFFFFu;

BONDRIVER_API IBonDriver2 *CreateBonDriver();
BONDRIVER_API const STRUCT_IBONDRIVER *CreateBonStruct();
