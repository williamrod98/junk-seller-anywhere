#pragma once
// agh/runtime/ImageRange.h -- bounded, fault-free proof read inside the loaded
// game image (E1 design r3).
//
// Shipped shared header, moved unchanged in behaviour from the
// design prototype (the E1 design r3.1 section 1.9). ASCII
// only, no CommonLib, no Windows header: host g++ builds and tests it.
//
// Why: an access violation is an SEH exception; a C++ catch (...) built with
// /EHsc does not see it. So nothing here relies on catch for memory safety.
// Every byte is read only after it is PROVEN to lie in committed, readable
// memory of the running image, with no integer or pointer overflow:
//   1. the image facts (SizeOfImage, sections) come from the PE headers of the
//      LOADED image, and every header byte is itself read through the same
//      page-checked reader;
//   2. the proof range [rva, rva + n) must be nonzero, must not overflow, must
//      end at or below SizeOfImage, and must lie inside ONE section whose
//      characteristics say readable AND executable, with that section itself
//      inside SizeOfImage;
//   3. base + rva + n must not overflow;
//   4. every page the range touches must be committed and readable according
//      to the page query (VirtualQuery in the adapter), walked region by
//      region; a guard page, PAGE_NOACCESS, a reserved or free page refuses.
// Only then is the memory copied. A refusal is a Reason the ledger writes as a
// quiet receipt (AL_PROOF_RANGE); the plugin stays inert.
//
// Image contract (the adapter binds VirtualQuery + memcpy, host tests a fake):
//   PageInfo Query(std::uint64_t address) const noexcept;
//   void Copy(std::uint64_t address, std::uint8_t* dst, std::size_t n) const noexcept;
//        raw copy; called ONLY for a range RangeReadable() accepted.

#include "agh/runtime/RuntimeGate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace agh::rt {

inline constexpr std::uint64_t kU64Max = (std::numeric_limits<std::uint64_t>::max)();

// Page query result. The adapter fills it from MEMORY_BASIC_INFORMATION through
// ClassifyPage(); `ok` is false when the query itself failed.
struct PageInfo {
    bool ok{ false };
    std::uint64_t regionBase{ 0 };
    std::uint64_t regionSize{ 0 };
    bool readable{ false };  // committed, readable protection, not guard, not no-access
};

// Win32 constants (winnt.h values; spelled here so the pure core needs no Windows header).
namespace win {
inline constexpr std::uint32_t kMemCommit = 0x1000u;
inline constexpr std::uint32_t kPageNoAccess = 0x01u;
inline constexpr std::uint32_t kPageReadOnly = 0x02u;
inline constexpr std::uint32_t kPageReadWrite = 0x04u;
inline constexpr std::uint32_t kPageWriteCopy = 0x08u;
inline constexpr std::uint32_t kPageExecute = 0x10u;  // execute-only: NOT readable
inline constexpr std::uint32_t kPageExecuteRead = 0x20u;
inline constexpr std::uint32_t kPageExecuteReadWrite = 0x40u;
inline constexpr std::uint32_t kPageExecuteWriteCopy = 0x80u;
inline constexpr std::uint32_t kPageGuard = 0x100u;
inline constexpr std::uint32_t kReadable = kPageReadOnly | kPageReadWrite | kPageWriteCopy | kPageExecuteRead |
                                           kPageExecuteReadWrite | kPageExecuteWriteCopy;
inline constexpr std::uint32_t kScnMemExecute = 0x20000000u;  // IMAGE_SCN_MEM_EXECUTE
inline constexpr std::uint32_t kScnMemRead = 0x40000000u;     // IMAGE_SCN_MEM_READ
}  // namespace win

// The same rule as UC HookCdr.cpp:15-26 and JS Oracle.h:127-133 (shipped).
[[nodiscard]] constexpr bool PageReadable(std::uint32_t state, std::uint32_t protect) noexcept
{
    return state == win::kMemCommit && (protect & (win::kPageGuard | win::kPageNoAccess)) == 0 &&
           (protect & win::kReadable) != 0;
}

[[nodiscard]] constexpr PageInfo ClassifyPage(bool queried, std::uint64_t regionBase, std::uint64_t regionSize,
    std::uint32_t state, std::uint32_t protect) noexcept
{
    return PageInfo{ queried, regionBase, regionSize, queried && PageReadable(state, protect) };
}

// True only if every byte of [address, address + n) lies in committed, readable
// regions. Walks the regions (a range may span two regions whose protections
// differ, e.g. a page another mod left RWX inside RX .text). Bounded loop.
template <class Pages>
[[nodiscard]] constexpr bool RangeReadable(const Pages& pages, std::uint64_t address, std::uint64_t n) noexcept
{
    if (n == 0 || address == 0 || address > kU64Max - n) { return false; }
    const std::uint64_t end = address + n;
    std::uint64_t cur = address;
    for (std::uint64_t guard = 0; cur < end; ++guard) {
        if (guard > n) { return false; }  // cannot need more regions than bytes
        const PageInfo q = pages.Query(cur);
        if (!q.ok || !q.readable || q.regionSize == 0 || q.regionBase > cur) { return false; }
        const std::uint64_t regionEnd = q.regionBase > kU64Max - q.regionSize ? kU64Max : q.regionBase + q.regionSize;
        if (regionEnd <= cur) { return false; }
        cur = regionEnd;
    }
    return true;
}

template <class Image>
[[nodiscard]] constexpr bool BoundedRead(const Image& image, std::uint64_t address, std::uint8_t* dst, std::size_t n) noexcept
{
    if (!RangeReadable(image, address, n)) { return false; }
    image.Copy(address, dst, n);
    return true;
}

// ---------------------------------------------------------------------------
// PE32+ headers of the loaded image (offsets: PE/COFF specification).
inline constexpr std::size_t kMaxSections = 96;          // the PE loader's own limit
inline constexpr std::uint32_t kMaxHeaderOffset = 0x1000;  // e_lfanew sanity bound (first page)

struct ImageSection {
    std::uint32_t va{ 0 };
    std::uint32_t vsize{ 0 };
    std::uint32_t characteristics{ 0 };
};

enum class ImageError : std::uint8_t {
    kNone,
    kBaseZero,
    kHeaderUnreadable,  // a header byte is not in committed readable memory
    kNoMz,
    kBadLfanew,
    kNoPe,
    kBadMachine,
    kBadSectionCount,
    kBadOptionalHeader,
    kBadSizes,          // SizeOfImage 0, SizeOfHeaders past SizeOfImage, section table past SizeOfHeaders
    kBaseOverflow,      // base + SizeOfImage wraps
};

struct ImageFacts {
    ImageError error{ ImageError::kBaseZero };
    std::uint64_t base{ 0 };
    std::uint32_t sizeOfImage{ 0 };
    std::uint32_t sizeOfHeaders{ 0 };
    std::uint16_t sectionCount{ 0 };
    std::array<ImageSection, kMaxSections> sections{};
    [[nodiscard]] constexpr bool Ok() const noexcept { return error == ImageError::kNone; }
};

namespace detail {
[[nodiscard]] constexpr std::uint16_t U16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
[[nodiscard]] constexpr std::uint32_t U32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
// base + off with an overflow check; every header address goes through it.
[[nodiscard]] constexpr bool At(std::uint64_t base, std::uint64_t off, std::uint64_t n, std::uint64_t& out) noexcept
{
    if (off > kU64Max - n || base > kU64Max - (off + n)) { return false; }
    out = base + off;
    return true;
}
}  // namespace detail

template <class Image>
[[nodiscard]] constexpr ImageFacts ReadImageFacts(const Image& image, std::uint64_t base) noexcept
{
    ImageFacts f;
    f.base = base;
    if (base == 0) { return f; }
    std::uint8_t dos[64]{};
    std::uint64_t addr = 0;
    const auto fail = [&](ImageError e) {
        f.error = e;
        return f;
    };
    if (!detail::At(base, 0, sizeof(dos), addr) || !BoundedRead(image, addr, dos, sizeof(dos))) {
        return fail(ImageError::kHeaderUnreadable);
    }
    if (dos[0] != 'M' || dos[1] != 'Z') { return fail(ImageError::kNoMz); }
    const std::uint32_t lfanew = detail::U32(dos + 0x3C);
    if (lfanew < sizeof(dos) || lfanew > kMaxHeaderOffset) { return fail(ImageError::kBadLfanew); }
    // Signature (4) + IMAGE_FILE_HEADER (20) + the optional header fields we need (64).
    std::uint8_t nt[4 + 20 + 64]{};
    if (!detail::At(base, lfanew, sizeof(nt), addr) || !BoundedRead(image, addr, nt, sizeof(nt))) {
        return fail(ImageError::kHeaderUnreadable);
    }
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) { return fail(ImageError::kNoPe); }
    if (detail::U16(nt + 4) != 0x8664) { return fail(ImageError::kBadMachine); }
    const std::uint16_t count = detail::U16(nt + 6);
    if (count == 0 || count > kMaxSections) { return fail(ImageError::kBadSectionCount); }
    const std::uint16_t optSize = detail::U16(nt + 20);
    const std::uint8_t* opt = nt + 24;
    if (optSize < 64 || detail::U16(opt) != 0x20B) { return fail(ImageError::kBadOptionalHeader); }
    f.sizeOfImage = detail::U32(opt + 56);
    f.sizeOfHeaders = detail::U32(opt + 60);
    const std::uint64_t tableOff = static_cast<std::uint64_t>(lfanew) + 24u + optSize;
    const std::uint64_t tableEnd = tableOff + 40u * count;
    if (f.sizeOfImage == 0 || f.sizeOfHeaders > f.sizeOfImage || tableEnd > f.sizeOfHeaders) {
        return fail(ImageError::kBadSizes);
    }
    std::uint64_t last = 0;
    if (!detail::At(base, 0, f.sizeOfImage, last)) { return fail(ImageError::kBaseOverflow); }
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint8_t sec[40]{};
        if (!detail::At(base, tableOff + 40u * i, sizeof(sec), addr) || !BoundedRead(image, addr, sec, sizeof(sec))) {
            return fail(ImageError::kHeaderUnreadable);
        }
        f.sections[i] = ImageSection{ detail::U32(sec + 12), detail::U32(sec + 8), detail::U32(sec + 36) };
    }
    f.sectionCount = count;
    f.error = ImageError::kNone;
    return f;
}

enum class ProofRange : std::uint8_t {
    kOk,
    kZero,             // rva == 0 (id resolved to nothing)
    kOverflow,         // rva + n wraps
    kOutsideImage,     // rva + n > SizeOfImage
    kPointerOverflow,  // base + rva + n wraps
    kNoExecSection,    // not inside one readable + executable section that itself lies in the image
    kImageUnknown,     // the image facts could not be read
};

[[nodiscard]] constexpr ProofRange CheckProofRange(const ImageFacts& f, std::uint64_t rva, std::uint64_t n) noexcept
{
    if (!f.Ok()) { return ProofRange::kImageUnknown; }
    if (rva == 0) { return ProofRange::kZero; }
    if (n == 0 || rva > kU64Max - n) { return ProofRange::kOverflow; }
    const std::uint64_t end = rva + n;
    if (end > f.sizeOfImage) { return ProofRange::kOutsideImage; }
    if (f.base > kU64Max - end) { return ProofRange::kPointerOverflow; }
    for (std::uint16_t i = 0; i < f.sectionCount; ++i) {
        const auto& s = f.sections[i];
        const std::uint32_t need = win::kScnMemRead | win::kScnMemExecute;
        if ((s.characteristics & need) != need || s.vsize == 0) { continue; }
        const std::uint64_t sEnd = static_cast<std::uint64_t>(s.va) + s.vsize;  // no wrap: both are 32-bit
        if (sEnd > f.sizeOfImage) { continue; }
        if (rva >= s.va && end <= sEnd) { return ProofRange::kOk; }
    }
    return ProofRange::kNoExecSection;
}

// The offline id -> RVA pin of a DLL's Q4 proof id (design section 4.1 SiteRow.rvaPin
// shape). rva == 0 = no pin for that runtime yet.
struct ProofPin {
    RuntimeId runtime{ RuntimeId::kNone };
    std::uint64_t id{ 0 };
    std::uint64_t rva{ 0 };
};

struct ProofTrace {  // what the core did (host tests assert on it)
    bool lookedUp{ false };
    bool pinChecked{ false };
    bool commonLibAsked{ false };
    bool imageParsed{ false };
    bool bytesCopied{ false };
    ProofRange range{ ProofRange::kImageUnknown };
};

// The whole Q4 proof, pure. The adapter binds:
//   lookup(id)          non-fatal id -> RVA (format 5: the dense entry from the file;
//                       formats 1/2: one REL::Offset2ID snapshot); 0 = absent; noexcept
//   commonLibOffset(id) REL::ID(id).offset(); called only after lookup found the id; noexcept
//   image               VirtualQuery + memcpy over the running process
// Order: presence -> offline pin -> CommonLib agreement -> image facts -> range ->
// page-checked copy. Nothing reads game memory before the range is proven.
template <class Lookup, class CommonLibOffset, class Image>
[[nodiscard]] constexpr Reason ProveAlUseCore(const Decision& d, std::int32_t alFormat, std::uint64_t id,
    const ProofPin* pins, std::size_t pinCount, Lookup&& lookup, CommonLibOffset&& commonLibOffset,
    const Image& image, std::uint64_t moduleBase, AlProof& proof, ProofTrace* trace = nullptr) noexcept
{
    ProofTrace local;
    ProofTrace& t = trace ? *trace : local;
    if (!EmitsAlProof(d)) { return Reason::kOk; }  // 1.6.1170: nothing at all
    proof.format = alFormat;
    proof.id = id;
    t.lookedUp = true;
    const std::uint64_t rva = lookup(id);
    if (rva == 0) { return Reason::kAlIdAbsent; }
    proof.rva = rva;
    for (std::size_t i = 0; i < pinCount; ++i) {
        if (pins[i].runtime == d.row->id && pins[i].id == id && pins[i].rva != 0) {
            t.pinChecked = true;
            if (pins[i].rva != rva) { return Reason::kAlPinMismatch; }
        }
    }
    t.commonLibAsked = true;
    proof.resolvedMatches = commonLibOffset(id) == rva;
    if (!proof.resolvedMatches) { return Reason::kAlIdAbsent; }
    t.imageParsed = true;
    const ImageFacts facts = ReadImageFacts(image, moduleBase);
    t.range = CheckProofRange(facts, rva, kAlProofBytes);
    if (t.range != ProofRange::kOk) { return Reason::kAlProofRange; }
    if (!BoundedRead(image, moduleBase + rva, proof.bytes.data(), kAlProofBytes)) { return Reason::kAlProofRange; }
    t.bytesCopied = true;
    proof.bytesRead = kAlProofBytes;
    return Reason::kOk;
}

}  // namespace agh::rt
