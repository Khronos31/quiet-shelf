// Host-side ABI declarations for the Windows ABI probe.
//
// Independent copy of the EDCB / LibISDB compatible ABI declarations used by
// the consumer harness.  It deliberately does NOT include the driver-side
// header so that layout and RTTI compatibility are validated, not assumed.
// The declarations are identical to Khronos31/BonDriver_Userland
// 60651d3eaf232b129b0a94f1c4dea373a075bd9c
// tests/abi_consumer/consumer_abi.h.
#pragma once

#if defined(_WIN32)
#  include <windows.h>
typedef WCHAR BON16CHAR;
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

#ifndef TRUE
#  define TRUE 1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

class IBonDriver
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

class IBonDriver2 : public IBonDriver
{
public:
	virtual const BON16CHAR *GetTunerName(void) = 0;
	virtual const BOOL IsTunerOpening(void) = 0;
	virtual const BON16CHAR *EnumTuningSpace(const DWORD dwSpace) = 0;
	virtual const BON16CHAR *EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) = 0;
	virtual const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) = 0;
	virtual const DWORD GetCurSpace(void) = 0;
	virtual const DWORD GetCurChannel(void) = 0;
	virtual void Release(void) = 0;
};

struct STRUCT_IBONDRIVER2
{
	STRUCT_IBONDRIVER st;
	const BON16CHAR *(*pF10)(void *);
	BOOL (*pF11)(void *);
	const BON16CHAR *(*pF12)(void *, DWORD);
	const BON16CHAR *(*pF13)(void *, DWORD, DWORD);
	BOOL (*pF14)(void *, DWORD, DWORD);
	DWORD (*pF15)(void *);
	DWORD (*pF16)(void *);
};
