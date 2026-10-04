#pragma once
#include "test_core_api_fakes.h"
namespace coretest {
void ScenarioExtensions(WarpDevice &, IOfpsCore *, FakeHost &, HostFrame &);
void ScenarioComputeWarp(WarpDevice &, IOfpsCore *, HostFrame &);
void ScenarioComputeTransferCore(WarpDevice &, IOfpsCore *, HostFrame &);
void ScenarioPlanDeferral(WarpDevice &, IOfpsCore *, FakeHost &, HostFrame &); // test_core_api_plan_defer.cpp
void ScenarioMenuQueueRelease(WarpDevice &, IOfpsCore *, FakeHost &, HostFrame &); // test_core_api_menu_queue.cpp
void ScenarioMenuRebuild(WarpDevice &, IOfpsCore *, HostFrame &);                  // test_core_api_menu_rebuild.cpp
void ScenarioTemporalGrid(WarpDevice &, IOfpsCore *, FakeHost &, HostFrame &);     // test_core_api_grid.cpp
#ifndef OFPS_TEST_DLL
void ScenarioComputeDirect(WarpDevice &, HostFrame &);
void ScenarioComputeTransfer(WarpDevice &);
#endif
}
