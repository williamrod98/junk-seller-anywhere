#pragma once
// agh/runtime/RuntimeGate.h -- engine-free runtime gate core (E1 design).
//
// Shipped shared header, moved unchanged in behaviour from the
// design prototype (design r3.1, sections 1.2-1.5).
// Contract and change rule: the shared runtime notes.
//
// Rules for this header (design section 1.2):
// - No CommonLib, no Windows header, no RE::/REL::/SKSE:: name. Host g++ builds it.
// - ASCII only (MSVC reads UTF-8 comments as cp932).
// - Pure: every function is a function of its arguments. File IO lives in
//   the adapter (SkseGate.h), which hands CheckAlHeader() a reader object.
// - The table below is the ONLY place a runtime, an SKSE floor or an Address
//   Library file name is written. Every DLL derives its declaration, gate,
//   AL precheck and receipt strings from it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace agh::rt {

// Gate revision, written into refusal rows so a receipt names the rule set.
// r2: AL header check covers the whole format 1/2 variable header.
// r3: bounded proof read (AL_PROOF_RANGE, AL_PIN_MISMATCH), compound steps and
//     the optional "sub" field of load_failure rows.
inline constexpr std::string_view kGateRevision = "agh-rt-3";

// Same packing as REL::Version::pack() (CommonLibSSE-NG v9.1.0
// include/REL/Version.h:69-75) and SKSE's packed skseVersion / runtimeVersion
// (include/SKSE/Interfaces.h:143-160): 8.8.12.4 bits.
[[nodiscard]] constexpr std::uint32_t Pack(std::uint32_t major, std::uint32_t minor,
    std::uint32_t patch, std::uint32_t build = 0) noexcept
{
    return ((major & 0xFFu) << 24) | ((minor & 0xFFu) << 16) | ((patch & 0xFFFu) << 4) | (build & 0xFu);
}

enum class RuntimeId : std::uint8_t { kNone = 0, k1_6_1170 = 1, k1_7_104 = 2 };

struct RuntimeRow {
    RuntimeId id;
    std::uint32_t runtime;      // packed, exact match only
    std::uint32_t skseFloor;    // packed, SKSE >= floor (ruling D3)
    std::string_view label;     // "1.6.1170"   (3 parts, receipts that print 3 parts)
    std::string_view label4;    // "1.6.1170.0" (4 parts, receipts that print 4 parts)
    std::string_view alBin;     // path CommonLib opens, relative to the game root
};

// The exact runtime table (ruling D1: exactly {1.6.1170, 1.7.104}).
// alBin mirrors CommonLib v9.1.0 IDDB::load(): AE family ->
// "Data/SKSE/Plugins/versionlib-<ver>.bin" with Version::string("-")
// (include/REL/IDDB.h:274-280, include/REL/Version.h:83; 1.7.x is AE because
// minor >= 6, include/REL/Module.h:337-349).
inline constexpr std::array<RuntimeRow, 2> kRuntimeTable{ {
    { RuntimeId::k1_6_1170, Pack(1, 6, 1170), Pack(2, 2, 6), "1.6.1170", "1.6.1170.0",
        "Data/SKSE/Plugins/versionlib-1-6-1170-0.bin" },
    { RuntimeId::k1_7_104, Pack(1, 7, 104), Pack(2, 3, 1), "1.7.104", "1.7.104.0",
        "Data/SKSE/Plugins/versionlib-1-7-104-0.bin" },
} };

// SKSEPlugin_Version.MinimumRequiredXSEVersion: the lowest floor in the table
// (a higher value would make SKSE itself refuse 1.6.1170 with its dialog).
[[nodiscard]] constexpr std::uint32_t MinimumSkseDeclared() noexcept
{
    std::uint32_t low = 0xFFFFFFFFu;
    for (const auto& row : kRuntimeTable) { low = row.skseFloor < low ? row.skseFloor : low; }
    return low;
}
static_assert(MinimumSkseDeclared() == Pack(2, 2, 6));

// Refusal reasons. The string codes are a receipt contract: never renamed,
// only appended (design section 1.4).
enum class Reason : std::uint8_t {
    kOk = 0,
    kNoInterface,           // SKSE passed a null LoadInterface
    kEditor,                // loaded by the Creation Kit
    kRuntimeUnsupported,    // runtime not in kRuntimeTable
    kSkseTooOld,            // runtime listed, SKSE below the row's floor
    kRuntimeModuleMismatch, // SKSE runtime != SkyrimSE.exe file version CommonLib will use
    kAlMissing,             // AL file absent or not a regular file
    kAlUnreadable,          // AL file present but its size or its 4-byte format field cannot be read
    kAlFormatUnsupported,   // header format not in {1, 2, 5} (CommonLib accepts exactly these)
    kAlVersionMismatch,     // header version != runtime
    kAlTruncated,           // file ends inside the header, or before count * minimum entry size
    kAlHeaderInvalid,       // r2: nameLen < 0, pointer size <= 0 or count <= 0 (values CommonLib mishandles)
    kAlIdAbsent,            // r2: the DLL's Q4 proof id is absent from a structurally valid bin
    kAlProofRange,          // r3: the proof id's RVA + 16 is not proven inside a readable executable
                            //     section of the loaded image (ImageRange.h); nothing was read
    kAlPinMismatch,         // r3: the bin's RVA for the proof id differs from the DLL's offline pin
};

[[nodiscard]] constexpr std::string_view ReasonCode(Reason reason) noexcept
{
    switch (reason) {
    case Reason::kOk: return "OK";
    case Reason::kNoInterface: return "NO_INTERFACE";
    case Reason::kEditor: return "EDITOR";
    case Reason::kRuntimeUnsupported: return "RUNTIME_UNSUPPORTED";
    case Reason::kSkseTooOld: return "SKSE_TOO_OLD";
    case Reason::kRuntimeModuleMismatch: return "RUNTIME_MODULE_MISMATCH";
    case Reason::kAlMissing: return "AL_MISSING";
    case Reason::kAlUnreadable: return "AL_UNREADABLE";
    case Reason::kAlFormatUnsupported: return "AL_FORMAT_UNSUPPORTED";
    case Reason::kAlVersionMismatch: return "AL_VERSION_MISMATCH";
    case Reason::kAlTruncated: return "AL_TRUNCATED";
    case Reason::kAlHeaderInvalid: return "AL_HEADER_INVALID";
    case Reason::kAlIdAbsent: return "AL_ID_ABSENT";
    case Reason::kAlProofRange: return "AL_PROOF_RANGE";
    case Reason::kAlPinMismatch: return "AL_PIN_MISMATCH";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr const RuntimeRow* FindRuntime(std::uint32_t runtime) noexcept
{
    for (const auto& row : kRuntimeTable) {
        if (row.runtime == runtime) { return &row; }
    }
    return nullptr;
}

// What SKSE told us at SKSEPlugin_Load (the adapter fills it from LoadInterface).
struct LoadFacts {
    bool hasInterface{ false };
    bool isEditor{ false };
    std::uint32_t runtime{ 0 };  // packed LoadInterface::RuntimeVersion()
    std::uint32_t skse{ 0 };     // packed LoadInterface::SKSEVersion()
};

struct Decision {
    Reason reason{ Reason::kNoInterface };
    const RuntimeRow* row{ nullptr };  // non-null once the runtime matched a row
    std::uint32_t runtime{ 0 };
    std::uint32_t skse{ 0 };
    [[nodiscard]] constexpr bool Passed() const noexcept { return reason == Reason::kOk; }
};

// Stage 1: interface, editor, exact runtime, per-runtime SKSE floor.
[[nodiscard]] constexpr Decision DecideLoad(const LoadFacts& facts) noexcept
{
    Decision d;
    d.runtime = facts.runtime;
    d.skse = facts.skse;
    if (!facts.hasInterface) { d.reason = Reason::kNoInterface; return d; }
    if (facts.isEditor) { d.reason = Reason::kEditor; return d; }
    d.row = FindRuntime(facts.runtime);
    if (!d.row) { d.reason = Reason::kRuntimeUnsupported; return d; }
    if (facts.skse < d.row->skseFloor) { d.reason = Reason::kSkseTooOld; return d; }
    d.reason = Reason::kOk;
    return d;
}

// Stage 2: CommonLib names the AL file from REL::Module's exe file version,
// not from SKSE's runtime value. If the two ever differ, stage 3 would check a
// different file than CommonLib opens, so refuse.
[[nodiscard]] constexpr Decision CheckModule(Decision d, std::uint32_t moduleVersion) noexcept
{
    if (d.Passed() && moduleVersion != d.runtime) { d.reason = Reason::kRuntimeModuleMismatch; }
    return d;
}

// ---------------------------------------------------------------------------
// Stage 3: the Address Library header, byte for byte as CommonLib v9.1.0 reads it.
//
// CommonLib's reader (include/REL/IDDB.h:144-185) opens the file with stream
// exceptions on badbit|failbit|eofbit (:160). Every short read therefore
// throws std::ios_base::failure, a std::system_error, which load_file()
// catches and turns into report_and_error(fail = true) = MessageBox +
// TerminateProcess (src/REL/IDDB.cpp:182-226, include/SKSE/Impl/PCH.h:539-599).
// The reads, in order:
//
//   all formats  int32 format                          IDDB.cpp:186-187
//   format 1/2   int32 version[4]                      IDDB.h:194-196
//                int32 nameLen, ignore(nameLen)        IDDB.h:195,197-198
//                int32 pointerSize, int32 addressCount IDDB.h:200-201
//                version check "version mismatch"      IDDB.cpp:230-233
//                shared mapping of count * 16 bytes    IDDB.cpp:235-253 (count 0 -> mapping fails)
//                count records, each >= 1 type byte    IDDB.cpp:390-391 (short -> eof throw)
//                pointer size divides the offset       IDDB.cpp:425 (0 -> divide by zero)
//   format 5     uint32 version[4], char name[64],     IDDB.h:227-235
//                int32 pointerSize, dataFormat, count
//                version check                         IDDB.cpp:260-263
//                file size >= 96 + 4 * count           IDDB.cpp:274-295
//
// CheckAlHeader() refuses every one of these conditions before CommonLib
// runs, so no header CommonLib would reject can pass. It is stricter only on
// values no real Address Library contains (nameLen < 0, pointer size <= 0,
// count <= 0), each of which CommonLib either mishandles or cannot use.
// Residual (not decidable from the header, design 1.3): the CONTENT of the
// format 1/2 record stream (an unhandled type nibble, a record that needs
// more bytes than remain), a failed memory map, and an absent id that
// CommonLib itself resolves later.
namespace al {
inline constexpr std::uint64_t kFormatAt = 0;
inline constexpr std::uint64_t kVersionAt = 4;          // 4 x 32-bit parts, all formats
inline constexpr std::uint64_t kV2NameLenAt = 20;       // int32
inline constexpr std::uint64_t kV2NameAt = 24;          // nameLen bytes, then pointerSize, count
inline constexpr std::uint64_t kV2TailBytes = 8;        // int32 pointerSize + int32 addressCount
inline constexpr std::uint64_t kV2MinEntryBytes = 1;    // one type byte, both nibbles implicit
inline constexpr std::uint64_t kV5NameAt = 20;          // char[64]
inline constexpr std::uint64_t kV5PointerSizeAt = 84;
inline constexpr std::uint64_t kV5DataFormatAt = 88;
inline constexpr std::uint64_t kV5CountAt = 92;
inline constexpr std::uint64_t kV5HeaderBytes = 96;     // IDDB.cpp:274-280
inline constexpr std::uint64_t kV5EntryBytes = 4;       // dense uint32 offsets
}  // namespace al

struct AlFacts {
    std::int32_t format{ 0 };
    std::uint32_t version{ 0 };       // packed header version
    std::int32_t nameLen{ 0 };        // format 1/2
    std::int32_t pointerSize{ 0 };
    std::int32_t count{ 0 };          // format 1/2: addressCount, format 5: offsetCount
    std::uint64_t headerBytes{ 0 };   // offset of the first record / dense entry
    std::uint64_t fileSize{ 0 };
};

namespace detail {
[[nodiscard]] constexpr std::uint32_t LoadU32(const std::uint8_t* b) noexcept
{
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}
[[nodiscard]] constexpr std::int32_t LoadI32(const std::uint8_t* b) noexcept
{
    return static_cast<std::int32_t>(LoadU32(b));
}
// CommonLib narrows each version part to uint16 before comparing (IDDB.h:203-205, :237-239).
[[nodiscard]] constexpr bool VersionMatches(const std::uint8_t* parts16, std::uint32_t runtime, std::uint32_t* packedOut) noexcept
{
    std::uint32_t p[4]{};
    for (std::size_t i = 0; i < 4; ++i) { p[i] = LoadU32(parts16 + 4 * i) & 0xFFFFu; }
    *packedOut = Pack(p[0], p[1], p[2], p[3]);
    // Compare unpacked parts, not the packed value: Pack() masks the patch to
    // 12 bits, CommonLib compares whole uint16 parts.
    const std::uint32_t want[4] = { (runtime >> 24) & 0xFFu, (runtime >> 16) & 0xFFu, (runtime >> 4) & 0xFFFu,
        runtime & 0xFu };
    for (std::size_t i = 0; i < 4; ++i) {
        if (p[i] != want[i]) { return false; }
    }
    return true;
}
}  // namespace detail

// Reader contract (the adapter reads the real file, host tests read memory):
//   bool Exists() const;                         regular file present
//   bool Size(std::uint64_t& out) const;         false on an IO error
//   bool ReadAt(std::uint64_t at, std::uint8_t* dst, std::size_t n) const;
//                                                false unless all n bytes were read
template <class Reader>
[[nodiscard]] constexpr Reason CheckAlHeader(const RuntimeRow& row, const Reader& file, AlFacts* out = nullptr) noexcept
{
    AlFacts facts;
    const auto finish = [&](Reason r) {
        if (out) { *out = facts; }
        return r;
    };
    if (!file.Exists()) { return finish(Reason::kAlMissing); }
    std::uint64_t size = 0;
    if (!file.Size(size)) { return finish(Reason::kAlUnreadable); }
    facts.fileSize = size;
    std::uint8_t b[al::kV5HeaderBytes]{};
    if (!file.ReadAt(al::kFormatAt, b, 4)) { return finish(Reason::kAlUnreadable); }
    facts.format = detail::LoadI32(b);
    if (facts.format != 1 && facts.format != 2 && facts.format != 5) { return finish(Reason::kAlFormatUnsupported); }

    if (facts.format == 5) {
        // The whole fixed header is read before anything is checked (IDDB.h:225-240).
        if (!file.ReadAt(al::kVersionAt, b + al::kVersionAt, al::kV5HeaderBytes - al::kVersionAt)) {
            return finish(Reason::kAlTruncated);
        }
        facts.pointerSize = detail::LoadI32(b + al::kV5PointerSizeAt);
        facts.count = detail::LoadI32(b + al::kV5CountAt);
        facts.headerBytes = al::kV5HeaderBytes;
        if (!detail::VersionMatches(b + al::kVersionAt, row.runtime, &facts.version)) {
            return finish(Reason::kAlVersionMismatch);
        }
        // Negative: CommonLib sign-extends it to a huge size_t and fails the size test.
        // Zero: CommonLib accepts it, but the table can then resolve no id at all.
        if (facts.count <= 0) { return finish(Reason::kAlHeaderInvalid); }
        const std::uint64_t needed = al::kV5HeaderBytes + al::kV5EntryBytes * static_cast<std::uint64_t>(facts.count);
        if (size < needed) { return finish(Reason::kAlTruncated); }
        return finish(Reason::kOk);
    }

    // Formats 1 and 2: version[4] and nameLen first.
    if (!file.ReadAt(al::kVersionAt, b + al::kVersionAt, al::kV2NameAt - al::kVersionAt)) {
        return finish(Reason::kAlTruncated);
    }
    facts.nameLen = detail::LoadI32(b + al::kV2NameLenAt);
    // A negative length is skipped as zero by MSVC's istream::ignore(); no real bin has one.
    if (facts.nameLen < 0) { return finish(Reason::kAlHeaderInvalid); }
    const std::uint64_t tailAt = al::kV2NameAt + static_cast<std::uint64_t>(facts.nameLen);
    // ignore(nameLen) past the end sets eofbit, which throws; so does a short tail read.
    std::uint8_t tail[al::kV2TailBytes]{};
    if (tailAt > size || !file.ReadAt(tailAt, tail, al::kV2TailBytes)) { return finish(Reason::kAlTruncated); }
    facts.pointerSize = detail::LoadI32(tail);
    facts.count = detail::LoadI32(tail + 4);
    facts.headerBytes = tailAt + al::kV2TailBytes;
    if (!detail::VersionMatches(b + al::kVersionAt, row.runtime, &facts.version)) {
        return finish(Reason::kAlVersionMismatch);
    }
    if (facts.pointerSize <= 0 || facts.count <= 0) { return finish(Reason::kAlHeaderInvalid); }
    const std::uint64_t needed = facts.headerBytes + al::kV2MinEntryBytes * static_cast<std::uint64_t>(facts.count);
    if (size < needed) { return finish(Reason::kAlTruncated); }
    return finish(Reason::kOk);
}

// Format 5 only: the id's dense offset read straight from the file, the same
// rule as IDDB::id2offset() for SSEv5 (src/REL/IDDB.cpp:158-163) but without
// report_id_lookup_failure(): 0 = absent. Formats 1/2 return 0 here; the
// adapter uses a REL::Offset2ID snapshot for them (SkseGate.h FindIdOffset).
template <class Reader>
[[nodiscard]] constexpr std::uint64_t LookupDenseOffset(const Reader& file, const AlFacts& facts, std::uint64_t id) noexcept
{
    if (facts.format != 5 || facts.count <= 0 || id >= static_cast<std::uint64_t>(facts.count)) { return 0; }
    std::uint8_t e[al::kV5EntryBytes]{};
    if (!file.ReadAt(al::kV5HeaderBytes + al::kV5EntryBytes * id, e, al::kV5EntryBytes)) { return 0; }
    return detail::LoadU32(e);
}

// "2.2.6" (parts = 3) or "1.6.1170.0" (parts = 4) from a packed value.
[[nodiscard]] inline std::string FormatPacked(std::uint32_t packed, int parts)
{
    std::string s = std::to_string((packed >> 24) & 0xFFu) + "." + std::to_string((packed >> 16) & 0xFFu) + "." +
                    std::to_string((packed >> 4) & 0xFFFu);
    if (parts >= 4) { s += "." + std::to_string(packed & 0xFu); }
    return s;
}

// Receipt strings. On 1.6.1170 + SKSE 2.2.6 each is byte-identical to the
// literal the DLL prints today (design section 1.5 table).
[[nodiscard]] inline std::string RuntimeLabel(const Decision& d) { return FormatPacked(d.runtime, 3); }
[[nodiscard]] inline std::string RuntimeLabel4(const Decision& d) { return FormatPacked(d.runtime, 4); }
[[nodiscard]] inline std::string SkseLabel(const Decision& d) { return FormatPacked(d.skse, 3); }
[[nodiscard]] inline std::string SkseLabel4(const Decision& d) { return FormatPacked(d.skse, 4); }
// "RUNTIME_1.6.1170_SKSE_2.2.6" (ShieldWatch Watch.cpp:610, Junk Seller Plugin.cpp:44).
[[nodiscard]] inline std::string ReceiptToken(const Decision& d)
{
    return "RUNTIME_" + RuntimeLabel(d) + "_SKSE_" + SkseLabel(d);
}

// ---------------------------------------------------------------------------
// Q4 (design ruling): proof of Address Library use. It appears ONLY
// on the non-1.6.1170 path, so every 1.6.1170 row stays byte-identical to the
// previous build. alFormat is an observation, not a condition;
// the proof is the id the DLL resolves and uses, its RVA, and a read at it.
[[nodiscard]] constexpr bool EmitsAlProof(const Decision& d) noexcept
{
    return d.Passed() && d.row != nullptr && d.row->id != RuntimeId::k1_6_1170;
}

inline constexpr std::size_t kAlProofBytes = 16;
struct AlProof {
    std::int32_t format{ 0 };          // AlFacts::format (observation)
    std::uint64_t id{ 0 };             // the id the DLL resolves and then uses
    std::uint64_t rva{ 0 };            // non-fatal lookup (file for format 5, Offset2ID for 1/2)
    bool resolvedMatches{ false };     // REL::ID(id).offset() == rva (CommonLib agrees)
    std::array<std::uint8_t, kAlProofBytes> bytes{};  // read at module base + rva
    std::size_t bytesRead{ 0 };
};

// The JSON suffix a DLL inserts before the closing brace of its startup row.
// Empty on 1.6.1170, so that row keeps its previous bytes.
[[nodiscard]] inline std::string AlProofJson(const Decision& d, const AlProof& p)
{
    if (!EmitsAlProof(d)) { return {}; }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string rva;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((p.rva >> shift) & 0xFu);
        if (rva.empty() && nibble == 0 && shift != 0) { continue; }
        rva += kHex[nibble];
    }
    std::string bytes;
    for (std::size_t i = 0; i < p.bytesRead && i < kAlProofBytes; ++i) {
        bytes += kHex[p.bytes[i] >> 4];
        bytes += kHex[p.bytes[i] & 0xFu];
    }
    std::string s = ",\"alFormat\":" + std::to_string(p.format) + ",\"alProofId\":" + std::to_string(p.id) +
                    ",\"alProofRva\":\"0x" + rva + "\",\"alProofResolved\":\"" +
                    (p.resolvedMatches ? "MATCH" : "MISMATCH") + "\",\"alProofRead\":\"" +
                    (p.bytesRead == kAlProofBytes ? "OK" : "SHORT") + "\",\"alProofBytes\":\"" + bytes + "\"";
    return s;
}

// One JSON object, no trailing newline, for the refusal row a DLL appends to
// its own receipt. Only emitted on refusal; the pass path adds no row.
[[nodiscard]] inline std::string GateRefusalJson(const Decision& d, std::string_view plugin)
{
    std::string s = "{\"type\":\"runtime_gate\",\"status\":\"REFUSED\",\"reason\":\"";
    s += ReasonCode(d.reason);
    s += "\",\"plugin\":\"";
    for (char c : plugin) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) { continue; }
        s += c;
    }
    s += "\",\"runtime\":\"" + RuntimeLabel4(d) + "\",\"skse\":\"" + SkseLabel(d) + "\",\"alBin\":\"";
    if (d.row) { s += d.row->alBin; }
    s += "\",\"gate\":\"";
    s += kGateRevision;
    s += "\"}";
    return s;
}

// One JSON object for a failure AFTER the gate passed (ruling D1): the install
// ledger names the step and the reason (STEP_FAILED, EXCEPTION, FORCED_FAULT).
// r3: `sub` names the failed sub-call of a compound step; the key is present
// only when it is non-empty. Written through the DLL's writer when it is open,
// else one synchronous append.
[[nodiscard]] inline std::string LoadFailureJson(std::string_view plugin, std::string_view step, std::string_view reason,
    std::string_view sub = {})
{
    const auto clean = [](std::string_view in) {
        std::string s;
        for (char c : in) {
            if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) { continue; }
            s += c;
        }
        return s;
    };
    std::string s = "{\"type\":\"load_failure\",\"status\":\"INERT\",\"step\":\"" + clean(step) + "\",";
    if (!sub.empty()) { s += "\"sub\":\"" + clean(sub) + "\","; }
    s += "\"reason\":\"" + clean(reason) + "\",\"plugin\":\"" + clean(plugin) + "\",\"gate\":\"";
    s += kGateRevision;
    s += "\"}";
    return s;
}

}  // namespace agh::rt
