#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agh::junk_seller::result_text
{
	using Table = std::map<std::string, std::string>;

	struct ParseResult
	{
		Table entries;
		std::size_t errors{};
	};

	struct RenderResult
	{
		std::string text;
		std::size_t unknownPlaceholders{};
		bool usedEnglishFallback{};
	};

	inline void AppendUtf8(std::string& out, std::uint32_t codePoint)
	{
		if (codePoint <= 0x7F) {
			out.push_back(static_cast<char>(codePoint));
		} else if (codePoint <= 0x7FF) {
			out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
			out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		} else if (codePoint <= 0xFFFF) {
			out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
			out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
			out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
	}

	inline bool DecodeUtf16Le(std::string_view bytes, std::string& out)
	{
		out.clear();
		std::size_t offset = 0;
		if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
			offset = 2;
		}
		if ((bytes.size() - offset) % 2 != 0) {
			return false;
		}
		while (offset < bytes.size()) {
			const auto low = static_cast<unsigned char>(bytes[offset]);
			const auto high = static_cast<unsigned char>(bytes[offset + 1]);
			offset += 2;
			std::uint32_t codePoint = static_cast<std::uint32_t>(low | (high << 8));
			if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
				if (offset + 1 >= bytes.size()) {
					return false;
				}
				const auto nextLow = static_cast<unsigned char>(bytes[offset]);
				const auto nextHigh = static_cast<unsigned char>(bytes[offset + 1]);
				const auto lowSurrogate = static_cast<std::uint32_t>(nextLow | (nextHigh << 8));
				if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
					return false;
				}
				offset += 2;
				codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (lowSurrogate - 0xDC00);
			} else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
				return false;
			}
			AppendUtf8(out, codePoint);
		}
		return true;
	}

	inline ParseResult Parse(std::string_view utf16LeBytes)
	{
		ParseResult result;
		std::string decoded;
		if (!DecodeUtf16Le(utf16LeBytes, decoded)) {
			result.errors = 1;
			return result;
		}

		std::size_t start = 0;
		while (start < decoded.size()) {
			const auto end = decoded.find('\n', start);
			const auto lineEnd = end == std::string::npos ? decoded.size() : end;
			std::string_view line(decoded.data() + start, lineEnd - start);
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			if (!line.empty()) {
				const auto tab = line.find('\t');
				if (tab == std::string_view::npos || tab == 0 || line.front() != '$') {
					++result.errors;
				} else {
					const std::string key(line.substr(0, tab));
					const std::string value(line.substr(tab + 1));
					if (!result.entries.emplace(key, value).second) {
						++result.errors;
					}
				}
			}
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
		return result;
	}

	inline const Table& BuiltinEnglish()
	{
		static const Table table{
			{ "$AGH_JS_RESULT_PREFIX", "Junk Seller" },
			{ "$AGH_JS_RESULT_SOLD", "{p}: sold {n} items for {g} gold." },
			{ "$AGH_JS_RESULT_PARTIAL", "{p}: sold {n} of {m} for {g} gold; {r}." },
			{ "$AGH_JS_RESULT_NOTHING", "{p}: nothing to sell." },
			{ "$AGH_JS_RESULT_BLOCKED", "{p}: nothing sold; {r}." },
			{ "$AGH_JS_RESULT_UNCERTAIN", "{p}: sale result is uncertain; check your inventory and gold." },
			{ "$AGH_JS_REASON_MERCHANT_GOLD", "the merchant ran out of gold" },
			{ "$AGH_JS_REASON_LIMIT", "limit reached, press again" },
			{ "$AGH_JS_REASON_STOPPED", "stopped by a safety check" },
			{ "$AGH_JS_REASON_NO_GOLD", "the merchant has no gold" },
			{ "$AGH_JS_REASON_BUSY", "close the open dialog first" },
			{ "$AGH_JS_REASON_RUNNING", "a sale is already running" },
			{ "$AGH_JS_REASON_OFF", "turned off in the MCM" },
			{ "$AGH_JS_REASON_SETTINGS", "settings file unreadable" },
			{ "$AGH_JS_REASON_LATCHED", "safety stop, reopen the barter menu" },
			{ "$AGH_JS_REASON_UNAVAILABLE", "selling is unavailable now" },
			// v0.1.0 (design r3 section 6): the sell-anywhere lines.
			{ "$AGH_JS_RESULT_RETURNED", "{p}: nothing sold; your items were returned." },
			{ "$AGH_JS_REASON_COMBAT", "not in combat" },
			{ "$AGH_JS_REASON_WORLD_BUSY", "close menus and dialogue first" },
			{ "$AGH_JS_REASON_LOADSAVE", "not while saving or loading" },
			{ "$AGH_JS_REASON_PRICE", "the price cannot be read" },
			{ "$AGH_JS_REASON_LATCHED_WORLD", "safety stop, load a save to reset" },
			// v0.2.0 (the v0.2 update plan r2 section 4.3): the sold line
			// names the kinds a press cleared and the stolen units it sold.
			{ "$AGH_JS_RESULT_SOLD_CLEARED", "{p}: sold {n} items for {g} gold; {c} kinds cleared." },
			{ "$AGH_JS_RESULT_SOLD_STOLEN", "{p}: sold {n} for {g} gold; {c} kinds cleared ({s} stolen)." },
		};
		return table;
	}

	inline const std::set<std::string>& PlaceholderNames()
	{
		// v0.2.0: {c} kinds cleared and {s} stolen units sold.
		static const std::set<std::string> names{ "p", "n", "m", "g", "r", "c", "s" };
		return names;
	}

	inline RenderResult Substitute(std::string_view input, const Table& values)
	{
		RenderResult result;
		for (std::size_t pos = 0; pos < input.size();) {
			if (input[pos] != '{') {
				result.text.push_back(input[pos++]);
				continue;
			}
			const auto close = input.find('}', pos + 1);
			if (close == std::string_view::npos) {
				result.text.append(input.substr(pos));
				break;
			}
			const std::string name(input.substr(pos + 1, close - pos - 1));
			if (PlaceholderNames().find(name) == PlaceholderNames().end()) {
				++result.unknownPlaceholders;
				result.text.append(input.substr(pos, close - pos + 1));
			} else {
				const auto value = values.find(name);
				if (value == values.end()) {
					++result.unknownPlaceholders;
					result.text.append(input.substr(pos, close - pos + 1));
				} else {
					result.text.append(value->second);
				}
			}
			pos = close + 1;
		}
		return result;
	}

	inline RenderResult Render(std::string_view key, const Table& localized, const Table& english,
		const Table& placeholders)
	{
		const auto local = localized.find(std::string(key));
		const auto en = english.find(std::string(key));
		const auto builtin = BuiltinEnglish().find(std::string(key));
		const std::string* selected = nullptr;
		bool fallback = false;
		if (local != localized.end()) {
			selected = &local->second;
		} else if (en != english.end()) {
			selected = &en->second;
			fallback = true;
		} else if (builtin != BuiltinEnglish().end()) {
			selected = &builtin->second;
			fallback = true;
		}
		if (selected == nullptr) {
			return { std::string(key), 0, true };
		}
		auto rendered = Substitute(*selected, placeholders);
		if (rendered.unknownPlaceholders != 0 && !fallback) {
			if (en != english.end()) {
				rendered = Substitute(en->second, placeholders);
			} else if (builtin != BuiltinEnglish().end()) {
				rendered = Substitute(builtin->second, placeholders);
			}
			rendered.usedEnglishFallback = true;
		}
		rendered.usedEnglishFallback = rendered.usedEnglishFallback || fallback;
		return rendered;
	}

	inline bool IsResultOrReasonKey(std::string_view key)
	{
		return key.rfind("$AGH_JS_RESULT_", 0) == 0 || key.rfind("$AGH_JS_REASON_", 0) == 0;
	}

	inline bool ResultKeySetsEqual(const Table& left, const Table& right)
	{
		std::set<std::string> leftKeys;
		std::set<std::string> rightKeys;
		for (const auto& [key, value] : left) {
			(void)value;
			if (IsResultOrReasonKey(key)) leftKeys.insert(key);
		}
		for (const auto& [key, value] : right) {
			(void)value;
			if (IsResultOrReasonKey(key)) rightKeys.insert(key);
		}
		return leftKeys == rightKeys;
	}

	inline std::size_t Utf8Length(std::string_view text)
	{
		std::size_t count = 0;
		for (const unsigned char ch : text) {
			if ((ch & 0xC0) != 0x80) ++count;
		}
		return count;
	}

	inline std::vector<std::string> ValidateResultLengths(const Table& table, std::size_t maximum)
	{
		const auto prefix = table.find("$AGH_JS_RESULT_PREFIX");
		if (prefix == table.end()) return { "$AGH_JS_RESULT_PREFIX missing" };
		const Table placeholders{ { "p", prefix->second }, { "n", "999" }, { "m", "999" },
			{ "g", "99999" }, { "r", "" } };
		std::vector<std::string> failures;
		const auto check = [&](const std::string& key, const Table& values, const std::string& label) {
			const auto found = table.find(key);
			if (found == table.end()) {
				failures.push_back(label + " missing: " + key);
				return;
			}
			const auto rendered = Substitute(found->second, values);
			if (rendered.unknownPlaceholders != 0 || Utf8Length(rendered.text) > maximum) {
				failures.push_back(label + " too long or invalid: " + key);
			}
		};
		check("$AGH_JS_RESULT_SOLD", placeholders, "sold");
		// v0.2.0 (the v0.2 update plan r2 section 4.3): the two new sold
		// forms at the catch-up sizes: n = 99,999 units, g = 9,999,999 gold,
		// c = 999 kinds, s = 99,999 stolen units.
		const Table large{ { "p", prefix->second }, { "n", "99999" }, { "m", "99999" }, { "g", "9999999" },
			{ "r", "" }, { "c", "999" }, { "s", "99999" } };
		check("$AGH_JS_RESULT_SOLD_CLEARED", large, "sold cleared");
		check("$AGH_JS_RESULT_SOLD_STOLEN", large, "sold stolen");
		check("$AGH_JS_RESULT_NOTHING", placeholders, "nothing");
		check("$AGH_JS_RESULT_UNCERTAIN", placeholders, "uncertain");
		check("$AGH_JS_RESULT_RETURNED", placeholders, "returned");
		// v0.1.0: a sell-anywhere partial may name a world gate that ended
		// the run between slices (Convert.h ConvertStopReason: a save or load,
		// a menu or dialogue, combat, an unavailable player, or a settings
		// change: switched off or unreadable).
		const std::vector<std::string> partialReasons{
			"$AGH_JS_REASON_MERCHANT_GOLD", "$AGH_JS_REASON_LIMIT", "$AGH_JS_REASON_STOPPED",
			"$AGH_JS_REASON_COMBAT", "$AGH_JS_REASON_WORLD_BUSY", "$AGH_JS_REASON_LOADSAVE",
			"$AGH_JS_REASON_UNAVAILABLE", "$AGH_JS_REASON_OFF", "$AGH_JS_REASON_SETTINGS" };
		for (const auto& reasonKey : partialReasons) {
			const auto reason = table.find(reasonKey);
			if (reason == table.end()) {
				failures.push_back("partial reason missing: " + reasonKey);
				continue;
			}
			auto values = placeholders;
			values["r"] = reason->second;
			check("$AGH_JS_RESULT_PARTIAL", values, "partial");
		}
		const std::vector<std::string> blockedReasons{
			"$AGH_JS_REASON_NO_GOLD", "$AGH_JS_REASON_BUSY", "$AGH_JS_REASON_RUNNING",
			"$AGH_JS_REASON_OFF", "$AGH_JS_REASON_SETTINGS", "$AGH_JS_REASON_LATCHED",
			"$AGH_JS_REASON_UNAVAILABLE", "$AGH_JS_REASON_COMBAT", "$AGH_JS_REASON_WORLD_BUSY",
			"$AGH_JS_REASON_LOADSAVE", "$AGH_JS_REASON_PRICE", "$AGH_JS_REASON_LATCHED_WORLD",
			"$AGH_JS_REASON_STOPPED", "$AGH_JS_REASON_LIMIT" };
		for (const auto& reasonKey : blockedReasons) {
			const auto reason = table.find(reasonKey);
			if (reason == table.end()) {
				failures.push_back("blocked reason missing: " + reasonKey);
				continue;
			}
			auto values = placeholders;
			values["r"] = reason->second;
			check("$AGH_JS_RESULT_BLOCKED", values, "blocked");
		}
		return failures;
	}
}
