//===-- SystemInitializerFull.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SystemInitializerFull.h"
#include "lldb/API/SBCommandInterpreter.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/Host.h"
#include "lldb/Initialization/SystemInitializerCommon.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Utility/Timer.h"

// BEGIN SWIFT
#include "Plugins/ExpressionParser/Swift/SwiftREPL.h"
#include "Plugins/InstrumentationRuntime/SwiftRuntimeReporting/SwiftRuntimeReporting.h"
#include "Plugins/Language/Swift/SwiftLanguage.h"
#include "lldb/Symbol/SwiftASTContext.h"
#include "lldb/Target/SwiftLanguageRuntime.h"
// END SWIFT
#include "llvm/Support/TargetSelect.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#include "llvm/ExecutionEngine/MCJIT.h"
#pragma clang diagnostic pop

#include <string>

#define LLDB_PLUGIN(p) LLDB_PLUGIN_DECLARE(p)
#include "Plugins/Plugins.def"

using namespace lldb_private;

SystemInitializerFull::SystemInitializerFull() {}

SystemInitializerFull::~SystemInitializerFull() {}

// BEGIN SWIFT
static void SwiftInitialize() {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
  SwiftLanguage::Initialize();
  SwiftLanguageRuntime::Initialize();
  SwiftREPL::Initialize();
#endif
  SwiftASTContext::Initialize();
  SwiftRuntimeReporting::Initialize();
}

static void SwiftTerminate() {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
  SwiftLanguage::Terminate();
  SwiftLanguageRuntime::Terminate();
  SwiftREPL::Terminate();
#endif
  SwiftASTContext::Terminate();
  SwiftRuntimeReporting::Terminate();
}

llvm::Error SystemInitializerFull::Initialize() {
  if (auto e = SystemInitializerCommon::Initialize())
    return e;

  // Initialize LLVM and Clang
  llvm::InitializeAllTargets();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllDisassemblers();

  // BEGIN SWIFT
  ::SwiftInitialize();
  // END SWIFT

#define LLDB_PLUGIN(p) LLDB_PLUGIN_INITIALIZE(p);
#include "Plugins/Plugins.def"

  // Scan for any system or user LLDB plug-ins.
  PluginManager::Initialize();

  // The process settings need to know about installed plug-ins, so the
  // Settings must be initialized AFTER PluginManager::Initialize is called.
  Debugger::SettingsInitialize();

  return llvm::Error::success();
}

void SystemInitializerFull::Terminate() {
  static Timer::Category func_cat(LLVM_PRETTY_FUNCTION);
  Timer scoped_timer(func_cat, LLVM_PRETTY_FUNCTION);

  Debugger::SettingsTerminate();

  // Terminate and unload and loaded system or user LLDB plug-ins.
  PluginManager::Terminate();

#define LLDB_PLUGIN(p) LLDB_PLUGIN_TERMINATE(p);
#include "Plugins/Plugins.def"

  // Now shutdown the common parts, in reverse order.
  SystemInitializerCommon::Terminate();
}
