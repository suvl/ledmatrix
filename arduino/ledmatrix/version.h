// The firmware version, shown in /status and on the portal, and read by the build workflow (shared/firmware-update.md).
// MAJOR.MINOR.PATCH, digits only, each at most 4 digits. Change it only for a release: the board offers an update only
// when the published version is strictly newer than this one.
#pragma once
#ifndef FIRMWARE_VERSION  // a test build may override it with -DFIRMWARE_VERSION=\"x.y.z\"; the build workflow reads the line below
#define FIRMWARE_VERSION "0.11.0"
#endif
