#pragma once
// Lightweight debug logging helpers for SMS HD Pack code paths.
// No functional changes. Include this header where needed and opt-in by defining SMS_HD_DEBUG
// before including, or set it project-wide in build settings.
//
// Usage:
//   #define SMS_HD_DEBUG 1
//   #include "SMS/HdPacks/HdPackDebug.h"
//   HDLOG("Started recording");
//   HDLOG_TAG("Folders", "Ensured save folder exists: " + folder);
//
// Note: If SMS_HD_DEBUG is not defined, all macros compile to no-ops.

#include "pch.h"
#include <string>
#include "Shared/MessageManager.h"

#ifndef HDLOG_PREFIX
#define HDLOG_PREFIX "[SMS HD] "
#endif

#ifdef SMS_HD_DEBUG
    #define HDLOG(MSG)            MessageManager::Log(std::string(HDLOG_PREFIX) + (MSG))
    #define HDLOG_TAG(TAG, MSG)   MessageManager::Log(std::string(HDLOG_PREFIX) + "[" + (TAG) + "] " + (MSG))
#else
    #define HDLOG(MSG)            do {} while(0)
    #define HDLOG_TAG(TAG, MSG)   do {} while(0)
#endif
