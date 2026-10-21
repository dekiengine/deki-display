#pragma once

#ifdef _WIN32
#ifdef DEKI_DISPLAY_EXPORTS
#define DEKI_DISPLAY_API __declspec(dllexport)
#else
#define DEKI_DISPLAY_API __declspec(dllimport)
#endif
#else
#define DEKI_DISPLAY_API __attribute__((visibility("default")))
#endif
