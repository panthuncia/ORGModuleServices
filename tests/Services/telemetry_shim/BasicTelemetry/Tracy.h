#pragma once
// Test-only stand-in for BasicTelemetry's Tracy.h: telemetry is not under test, and the standalone services tests
// build without BasicTelemetry/Tracy. Only the macros the services sources use.
#define BT_ZONE_SCOPE(name) ((void)0)
#define BT_ZONE_NAMED(variable, name) ((void)0)
