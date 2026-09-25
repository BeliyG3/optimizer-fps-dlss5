#pragma once
#include "test_core_api_fakes.h"
namespace coretest {
void ScenarioExtensions(WarpDevice &, IOfpsCore *, FakeHost &, HostFrame &);
void ScenarioComputeWarp(WarpDevice &, IOfpsCore *, HostFrame &);
#ifndef OFPS_TEST_DLL
void ScenarioComputeDirect(WarpDevice &, HostFrame &);
#endif
}
