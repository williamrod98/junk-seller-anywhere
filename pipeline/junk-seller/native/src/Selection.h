#pragma once
// O-5 / B-05. The highlighted row is read through the standard ItemList only
// (J05/J06/J08/J09). Nothing is marked, stored or changed; the selected index is
// read back so a mismatch between the displayed order and items[] is visible.
#include "WObserve.h"
#include "receipt/Log.h"
#include <RE/E/ExtraUniqueID.h>  // CommonLibSSE-NG ExtraUniqueID.h:8-26.
#include <RE/I/InventoryChanges.h>  // CommonLibSSE-NG InventoryChanges.h:69-72.
namespace js {
// w-probe-r1 (design section 4.2): the identity of the highlighted row and the
// instant it was read are separated. The index is read on both sides of the
// entry read, so a selection that moved between the two halves of this row is
// visible as selectionValid=false instead of being reported as one reading.
// The uniqueID of the stack is deliberately NOT read: it would need a phase
// this build cannot defend (design section 2.2), so the row says
// READ_PHASE_UNPROVEN and identityQuality stays base_only at best. The source
// names and the reason codes live in WObserve.h with the rules that use them.
inline const char* ReadSelectedInner(RE::ItemList* list, SelectedInfo& info,
                                    std::uint32_t maxXListVisits) {
    if (!list) return "NO_ITEMLIST";
    info.listSize = static_cast<std::uint32_t>(list->items.size());
    RE::GFxValue index;
    if (list->root.GetMember("selectedIndex", &index) && index.IsNumber()) {
        info.indexRead = true;
        info.selectedIndexRead = static_cast<std::int32_t>(index.GetNumber());
        info.indexSource = SelectionIndexSource;
    }
    auto* item = list->GetSelectedItem();
    if (!item) return "NO_SELECTION";
    auto* entry = item->data.objDesc;
    if (!entry) return "NO_ENTRY_DATA";
    info.baseObj = entry->object ? entry->object->GetFormID() : 0;
    info.countDelta = entry->countDelta;
    // The walk stops at the cumulative run cap. A count the cap
    // cut short is marked truncated, so it never reads as a shorter stack.
    if (entry->extraLists) {
        for (auto* extra : *entry->extraLists) {
            if (!extra) continue;
            if (info.xListCount >= maxXListVisits) { info.xListTruncated = true; break; }
            ++info.xListCount;
            // Public ExtraDataList::GetByType<T> and ExtraUniqueID::uniqueID;
            // see vendor/commonlibsse-ng/include/RE/E/ExtraDataList.h:150-163 and
            // ExtraUniqueID.h:23-26. Read only; the pointer does not escape.
            if (!info.extraUniqueIdPresent) {
                if (const auto* unique = extra->GetByType<RE::ExtraUniqueID>()) {
                info.extraUniqueIdPresent = true;
                info.extraUniqueId = unique->uniqueID;
                }
            }
        }
    }
    info.extraDataListPresent = entry->extraLists && !entry->extraLists->empty();
    info.visits = info.xListCount;
    if (!maxXListVisits) info.visitCapReached = true;
    if (info.xListTruncated) info.visitCapReached = true;
    const char* display = entry->GetDisplayName();
    CopyText(info.displayName, display);
    info.value = entry->GetValue();
    info.soulLevel = static_cast<std::int32_t>(entry->GetSoulLevel());
    info.questObject = entry->IsQuestObject();
    info.favorite = entry->IsFavorited();
    info.worn = entry->IsWorn();
    info.enchanted = entry->IsEnchanted();
    // InventoryEntryData::CanItemBeTaken is declared in the pinned
    // CommonLibSSE-NG (800d61ee) include but has no src/ implementation
    // (link error LNK2019). No Address Library ID is guessed for it (design
    // doc SS3.1). Synthesized instead from the already-read exclusion flags:
    // called with (false,false,false) none of the noEquipped/noFavourited/
    // noQuestItem overrides are requested, so a quest object, a worn item or
    // a favorited item is treated as not takeable. This is an approximation
    // of the engine result, not the exact value; recorded read-only.
    info.canBeTaken = !info.questObject && !info.worn && !info.favorite;
    auto* owner = entry->GetOwner();
    info.ownerReadable = true;
    info.ownerForm = owner ? owner->GetFormID() : 0;
    auto* player = RE::PlayerCharacter::GetSingleton();
    info.ownerIsPlayer = owner && player && info.ownerForm == player->GetFormID();
    // Ownership is a protection input, not the stolen verdict (J09/U06).
    info.owned = player && entry->IsOwnedBy(player, true);
    // No public API here distinguishes a theft verdict from generic ownership.
    // Keep that absence explicit rather than treating IsOwnedBy as stolen.
    info.stolenKnown = false;
    // Count actual player InventoryChanges stacks, rather than treating the
    // selected entry's list length as an inventory-wide fact. The public
    // entryList member is InventoryChanges.h:69-72; each candidate entry and
    // each non-null xList visit consumes the same allowance as the selected
    // entry walk above. One matching non-selected InventoryEntryData counts
    // once even when it owns several xList nodes. The engine-free rule in
    // WObserve.h excludes the selected entry and makes a capped result a lower
    // bound.
    if (!info.xListTruncated && player && info.baseObj) {
        auto* changes = player->GetInventoryChanges(true);
        if (changes && changes->entryList) {
            MixedStackScan scan{
                info.baseObj,
                reinterpret_cast<std::uintptr_t>(entry),
                maxXListVisits,
                info.visits,
                info.mixedExtraListStacks,
            };
            for (auto* candidate : *changes->entryList) {
                const auto base = candidate && candidate->object
                                      ? candidate->object->GetFormID()
                                      : 0;
                if (!scan.VisitEntry(base, reinterpret_cast<std::uintptr_t>(candidate))) break;
                if (!candidate || !candidate->object || base != info.baseObj || !candidate->extraLists) {
                    continue;
                }
                for (auto* extra : *candidate->extraLists) {
                    if (!extra) continue;
                    if (!scan.VisitExtra(reinterpret_cast<std::uintptr_t>(extra))) break;
                }
                if (scan.truncated) break;
            }
            info.visits = scan.visits;
            info.mixedExtraListStacks = scan.mixedStacks;
            info.xListTruncated = scan.truncated;
            info.visitCapReached = info.visitCapReached || scan.truncated;
        }
    }
    // Shape and identity quality, both derived from values already read. No
    // uniqueID is fetched, so the best this row can claim is "base_only".
    // A count the cap cut short is not a shape: it says unknown rather than
    // "unextended" for a stack whose extra lists were never reached.
    info.instanceKind = InstanceKindName(!info.xListTruncated && !info.visitCapReached,
                                         info.xListCount);
    info.identityQuality = IdentityQualityName(true, info.baseObj, false);
    // Second index read. If the highlighted row moved while this row was being
    // built, the two halves describe two different selections and the row says
    // so instead of pretending to be one snapshot.
    RE::GFxValue after;
    if (list->root.GetMember("selectedIndex", &after) && after.IsNumber()) {
        info.selectedIndexAfter = static_cast<std::int32_t>(after.GetNumber());
    }
    if (!info.indexRead || info.selectedIndexAfter != info.selectedIndexRead) {
        return SelectionNotAtCall;
    }
    info.selectionValid = true;
    return "OK";
}
// The sample bounds belong to the read itself and are set on every path, so a
// row that gave up early still states the window it looked at rather than a
// start with no end.
inline const char* ReadSelected(RE::ItemList* list, SelectedInfo& info,
                                std::uint32_t maxXListVisits) {
    info.sampleStartNs = NowNs();
    info.source = SelectionSource;
    const char* reason = ReadSelectedInner(list, info, maxXListVisits);
    info.sampleEndNs = NowNs();
    return reason;
}
}
