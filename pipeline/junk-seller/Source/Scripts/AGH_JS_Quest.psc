Scriptname AGH_JS_Quest extends Quest

; Observation only. The quest reports that it is running so the O stage can tell
; "the plugin never started" apart from "the ESP never started". It reads no
; inventory, touches no container and changes nothing.
; A Quest script receives no load event of its own; the update heartbeat covers
; a reload instead of an actor alias.
;
; r3 (anomaly 2): probe-js01 produced no js_papyrus row in 8 min 44 s and
; no Papyrus log line either, so "the quest never ran" and "the SKSE gate said
; no" could not be told apart - the one thing this script exists to separate.
; Two changes: the ModEvent is no longer gated on SKSE.GetPluginVersion, and
; Debug.Trace records the quest's own liveness plus the version the gate would
; have read. Debug.Trace is vanilla, so the Papyrus log carries evidence even
; when nothing in SKSE answers. (The SEQ start list is the other half of the
; fix and lives in generate_js.py.)
;
; r4 (probe-js02 anomaly 1): probe-js02 proved the quest alive - 65 Debug.Trace
; lines, plugin version 16 - while js_papyrus stayed at 0 rows. The reason is
; the mechanism, not the timing: ModEvent.Create/Push*/Send carries an arbitrary
; argument list and SKSE delivers it straight to the Papyrus RegisterForModEvent
; registrations, so there is no SKSEModCallbackEvent for a DLL sink to receive
; ("ModEvent allows sending mod events with any number/type of arguments, unlike
; the more limited Form.SendModEvent" - ModEvent.psc). Form.SendModEvent is the
; one that dispatches a ModCallbackEvent, whose fields are exactly the callback
; signature (string eventName, string strArg, float numArg, Form sender). The
; heartbeat number lets the receipt see a dropped report as a gap.

Int Reports

Event OnInit()
    Report("OnInit")
    RegisterForSingleUpdate(5.0)
EndEvent

Event OnUpdate()
    Report("OnUpdate")
    RegisterForSingleUpdate(5.0)
EndEvent

Function Report(String phase)
    Debug.Trace("AGH_JS_Quest quest alive: " + phase)
    Int version = SKSE.GetPluginVersion("AGH_JunkSeller")
    Debug.Trace("AGH_JS_Quest plugin version: " + version)
    Reports = Reports + 1
    ; Self is the quest form, so the DLL row names the sender that reported.
    SendModEvent("AGH_JunkSeller", phase, Reports as Float)
EndFunction
