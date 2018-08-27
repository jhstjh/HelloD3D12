#pragma once
#include <stdio.h>

#ifdef _DEBUG
#define LOG_ERROR(...) printf(__VA_ARGS__)
#define HR_ERROR_CHECK_CALL(func, ret, ... ) \
        if (FAILED(func)) \
        {   \
            LOG_ERROR(__VA_ARGS__); \
            assert((#func) == "failed"); \
            return ret; \
        } 
#else
#define LOG_ERROR(...) void()
#define HR_ERROR_CHECK_CALL(func, ret, ... ) func
#endif