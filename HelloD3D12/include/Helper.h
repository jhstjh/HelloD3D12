#pragma once
#include <stdio.h>
#include <tchar.h>

#ifdef _DEBUG

inline void ErrorDescription(HRESULT hr)
{
    if (FACILITY_WINDOWS == HRESULT_FACILITY(hr))
        hr = HRESULT_CODE(hr);
    TCHAR* szErrMsg;

    if (FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&szErrMsg, 0, NULL) != 0)
    {
        _tprintf(TEXT("%s"), szErrMsg);
        LocalFree(szErrMsg);
    }
    else
        _tprintf(TEXT("[Could not find a description for error # %#x.]\n"), hr);
}

#define LOG_ERROR(...) printf(__VA_ARGS__)
#define HR_ERROR_CHECK_CALL(func, ret, ... ) { _HR_ERROR_CHECK_CALL(func, ret, __VA_ARGS__) }
#define _HR_ERROR_CHECK_CALL(func, ret, ... ) \
        auto hr = func; \
        if (FAILED(hr)) \
        {   \
            LOG_ERROR(__VA_ARGS__); \
            ErrorDescription(hr); \
            assert((#func) == "failed"); \
            return ret; \
        } 
#else
#define LOG_ERROR(...) void()
#define HR_ERROR_CHECK_CALL(func, ret, ... ) func
#endif