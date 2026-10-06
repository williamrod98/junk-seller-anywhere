#pragma once
#include <SKSE/SKSE.h>
#include "Install.h"
namespace js {
struct Row;
bool LoadSettings();
void InitProcessRun();
// w-probe-r71 (v0.9.0): the startup row takes its reason, runtime, skse
// and Q4 suffix from the shared gate's labels (Install.h LabelsFor).
void ConfigureStartupRow(Row& row, const e1::StartupLabels& labels);
// The runtime pair GetRuntime reports, from the same labels (set once at
// receipt_open, before any native can be bound).
void SetRuntimeLabels(const char* runtime, const char* skse) noexcept;
void OnMessage(SKSE::MessagingInterface::Message* message);
}
