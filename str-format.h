#pragma once

#include "str-common.h"
#include "str-convert.h"
#include "str-numbers.h"
#include "str-wire.h"

#include <string>
#include <type_traits>
#include <utility>
#include <iostream>

namespace str {
	/* formattable interface which requires:
	*	operator() to take a sink of any type, a value of the type, a utf32 string-view with formatting-string and a boolean return-value
	*	if the format-string was valid (should leave the sink untouched if the format is invalid; empty format should be valid at all times) */
	template <class Type>
	struct Formatter;
	template <class Type>
	concept IsFormattable = requires(const Type & val, const std::u32string_view & fmt, str::ChSmall<1>&cs, str::WdSmall<1>&ws, str::U8Small<1>&u8s, str::U16Small<1>&u16s, str::U32Small<1>&u32s) {
		{ str::Formatter<std::remove_cvref_t<Type>>{}(cs, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(ws, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u8s, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u16s, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u32s, val, fmt) } -> std::same_as<bool>;
	};

	/* wrapper to format into sink */
	template <class ChType>
	constexpr bool FormatSingle(str::IsSink<ChType> auto&& sink, const str::IsFormattable auto& val, const std::u32string_view& fmt = U"") {
		return str::Formatter<std::remove_cvref_t<decltype(val)>>{}(sink, val, fmt);
	}

	namespace detail {
		template <class ChType>
		constexpr int8_t FormatIndex(auto& sink, size_t index, const std::u32string_view& fmt) {
			return -1;
		}

		template <class ChType, class Arg, class... Args>
		constexpr int8_t FormatIndex(auto& sink, size_t index, const std::u32string_view& fmt, const Arg& arg, const Args&... args) {
			if (index > 0)
				return detail::FormatIndex<ChType, Args...>(sink, index - 1, fmt, args...);
			else
				return (str::FormatSingle<ChType>(sink, arg, fmt) ? 1 : 0);
		}

		template <class ChType, class Arg, class... Args>
		constexpr void Append(auto& sink, const Arg& arg, const Args&... args) {
			str::FormatSingle<ChType>(sink, arg, U"");
			if constexpr (sizeof...(args) > 0)
				detail::Append<ChType, Args...>(sink, args...);
		}

		template <class FmtType, class SinkType>
		constexpr bool FormatPrintUntilArg(auto& sink, std::basic_string_view<FmtType>& fmt) {
			/* iterate until the entire format-string has been processed or until an argument has been encountered */
			bool openStarted = false;
			while (!fmt.empty()) {
				/* decode the next character (handle invalid decodings as valid characters, just not written to the sink) */
				auto [cp, len] = str::Decode(fmt, true);

				/* check if an open-bracket has been encountered, which could either be part of an escape sequence or mark the start of an argument */
				if (cp == U'{')
					openStarted = !openStarted;

				/* check if an opening bracket was the last token, in which case this must have been a valid argument start */
				else if (openStarted)
					return true;

				/* check if the token should be committed to the sink (ignore any characters not being writable to the destination character-set) */
				if (!openStarted && cp::Valid(cp)) {
					if constexpr (str::EffSame<FmtType, SinkType>) {
						if (len == 1)
							str::SinkChars<SinkType>(sink, static_cast<SinkType>(fmt[0]));
						else
							str::SinkString<SinkType>(sink, reinterpret_cast<const SinkType*>(fmt.data()), len);
					}
					else
						str::EncodeInto(sink, cp);
				}
				fmt = fmt.substr(len);
			}
			return false;
		}

		struct NestedIndex {
			size_t index = 0;
			bool valid = false;
		};
		template <class FmtType>
		constexpr detail::NestedIndex FormatParseNestedIndex(size_t& argSequence, std::basic_string_view<FmtType>& fmt) {
			detail::NestedIndex out{};

			/* try to parse any leading numbers (ignore range errors as they will just result in the largest possible value) */
			auto [value, consumed, result] = str::ParseNum<size_t>(fmt);
			if (result == str::NumResult::valid || result == str::NumResult::range) {
				fmt = fmt.substr(consumed);
				out.index = value;
			}
			else
				out.index = argSequence++;

			/* parse the optional separator and closing bracket */
			for (size_t i = 0; i < 2; ++i) {
				auto [cp, len] = str::ReadAscii(fmt);
				fmt = fmt.substr(len);
				if (cp == U':' && i == 0)
					continue;
				if (cp != U'}')
					break;

				/* mark the index as valid and return it */
				out.valid = true;
				return out;
			}

			/* return the invalid result */
			return out;
		}
	}

	/* format the arguments into the sink, based on the formatting-string */
	constexpr auto& FormatInto(str::AnySink auto&& sink, const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		using FmtType = str::StringCharType<decltype(fmt)>;
		using SinkType = str::SinkCharType<decltype(sink)>;
		enum class ArgValid : uint8_t {
			valid,
			malformed,
			index,
			format
		};

		/* buffer the format-string until the entire format has been consumed */
		size_t argSequence = 0;
		std::u32string argFormatBuffer;
		std::basic_string_view<FmtType> view = str::StringView<FmtType>(fmt);

		/* print all characters from the format string until the next argument starts */
		while (detail::FormatPrintUntilArg<FmtType, SinkType>(sink, view)) {
			argFormatBuffer.clear();

			/* parse the argument-index (ignore range errors as they will just result in the largest possible value) */
			auto [argIndex, consumed, result] = str::ParseNum<size_t>(view);
			if (result == str::NumResult::valid || result == str::NumResult::range)
				view = view.substr(consumed);
			else
				argIndex = argSequence++;

			/* iterate over the remaining characters until the end of the argument has been reached */
			bool hasSeparator = false;
			ArgValid fmtState = ArgValid::valid;
			while (true) {
				auto [cp, len] = str::Decode(view, true);
				if (!cp::Valid(cp)) {
					fmtState = ArgValid::malformed;
					break;
				}
				view = view.substr(len);

				/* check if the end has been reached */
				if (cp == U'}')
					break;

				/* check if the separator has been encountered */
				else if (!hasSeparator) {
					if (cp != U':') {
						fmtState = ArgValid::malformed;
						break;
					}
					hasSeparator = true;
				}

				/* check if its any character which can just be written to the format-buffer */
				else if (cp != U'{')
					argFormatBuffer.push_back(cp);

				/* expand the nested argument into the format-buffer */
				else {
					auto [nestIndex, nValid] = detail::FormatParseNestedIndex<FmtType>(argSequence, view);
					if (!nValid) {
						fmtState = ArgValid::malformed;
						break;
					}

					/* check if the argument should be skipped as the current format-string already resulted in an issue */
					if (fmtState != ArgValid::valid)
						continue;

					/* try to write the index to the temporary argument buffer */
					int8_t res = detail::FormatIndex<char32_t>(argFormatBuffer, nestIndex, U"", args...);
					if (res != 1)
						fmtState = (res < 0 ? ArgValid::index : ArgValid::format);
				}
			}

			/* check if a well-formed argument has been found and print it */
			if (fmtState == ArgValid::valid) {
				int8_t res = detail::FormatIndex<SinkType>(sink, argIndex, argFormatBuffer, args...);
				if (res != 1)
					fmtState = (res < 0 ? ArgValid::index : ArgValid::format);
			}

			/* check if a format-string issue was encountered */
			if (fmtState == ArgValid::index)
				str::Append(sink, U"#index");
			else if (fmtState == ArgValid::format)
				str::Append(sink, U"#fmt");

			/* check if the entire format-string was malformed, in which case further arguments will not be processed */
			else if (fmtState == ArgValid::malformed) {
				str::Append(sink, U"#malformed");
				break;
			}
		}
		return sink;
	}

	/* format the arguments to a string of the destination character-type (returning std::basic_string) */
	template <str::IsChar ChType>
	constexpr std::basic_string<ChType> FormatTo(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		std::basic_string<ChType> out{};
		return str::FormatInto(out, fmt, args...);
	}

	/* format the arguments to a string of the destination character-type (returning str::Small<Capacity>) */
	template <str::IsChar ChType, intptr_t Capacity>
	constexpr str::Small<ChType, Capacity> FormatTo(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		str::Small<ChType, Capacity> out{};
		return str::FormatInto(out, fmt, args...);
	}

	/* convenience for fast formatting to a std::basic_string */
	constexpr std::string ChFormat(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char>(fmt, args...);
	}
	constexpr std::wstring WdFormat(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<wchar_t>(fmt, args...);
	}
	constexpr std::u8string U8Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char8_t>(fmt, args...);
	}
	constexpr std::u16string U16Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char16_t>(fmt, args...);
	}
	constexpr std::u32string U32Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char32_t>(fmt, args...);
	}

	/* convenience for fast formatting to a str::Small<Capacity> */
	template <intptr_t Capacity>
	constexpr str::ChSmall<Capacity> ChFormat(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char, Capacity>(fmt, args...);
	}
	template <intptr_t Capacity>
	constexpr str::WdSmall<Capacity> WdFormat(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<wchar_t, Capacity>(fmt, args...);
	}
	template <intptr_t Capacity>
	constexpr str::U8Small<Capacity> U8Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char8_t, Capacity>(fmt, args...);
	}
	template <intptr_t Capacity>
	constexpr str::U16Small<Capacity> U16Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char16_t, Capacity>(fmt, args...);
	}
	template <intptr_t Capacity>
	constexpr str::U32Small<Capacity> U32Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		return str::FormatTo<char32_t, Capacity>(fmt, args...);
	}

	/* build the arguments into the sink (as if formatting with format "{}{}{}...") */
	constexpr auto& BuildInto(str::AnySink auto&& sink, const str::IsFormattable auto&... args) {
		using ChType = str::SinkCharType<decltype(sink)>;
		if constexpr (sizeof...(args) > 0)
			detail::Append<ChType>(sink, args...);
		return sink;
	}

	/* build the arguments to a string of the destination character-type (returning std::basic_string) */
	template <str::IsChar ChType>
	constexpr std::basic_string<ChType> BuildTo(const str::IsFormattable auto&... args) {
		std::basic_string<ChType> out{};
		return str::BuildInto(out, args...);
	}

	/* build the arguments to a string of the destination character-type (returning str::Small<Capacity>) */
	template <str::IsChar ChType, intptr_t Capacity>
	constexpr str::Small<ChType, Capacity> BuildTo(const str::IsFormattable auto&... args) {
		str::Small<ChType, Capacity> out{};
		return str::BuildInto(out, args...);
	}

	/* convenience for fast building to a std::basic_string */
	constexpr std::string ChBuild(const str::IsFormattable auto&... args) {
		return str::BuildTo<char>(args...);
	}
	constexpr std::wstring WdBuild(const str::IsFormattable auto&... args) {
		return str::BuildTo<wchar_t>(args...);
	}
	constexpr std::u8string U8Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char8_t>(args...);
	}
	constexpr std::u16string U16Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char16_t>(args...);
	}
	constexpr std::u32string U32Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char32_t>(args...);
	}

	/* convenience for fast formatting to a str::Small<Capacity> */
	template <intptr_t Capacity>
	constexpr str::ChSmall<Capacity> ChBuild(const str::IsFormattable auto&... args) {
		return str::BuildTo<char, Capacity>(args...);
	}
	template <intptr_t Capacity>
	constexpr str::WdSmall<Capacity> WdBuild(const str::IsFormattable auto&... args) {
		return str::BuildTo<wchar_t, Capacity>(args...);
	}
	template <intptr_t Capacity>
	constexpr str::U8Small<Capacity> U8Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char8_t, Capacity>(args...);
	}
	template <intptr_t Capacity>
	constexpr str::U16Small<Capacity> U16Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char16_t, Capacity>(args...);
	}
	template <intptr_t Capacity>
	constexpr str::U32Small<Capacity> U32Build(const str::IsFormattable auto&... args) {
		return str::BuildTo<char32_t, Capacity>(args...);
	}

	/* convenience for fast formatting to std::cout */
	constexpr void Format(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatInto(std::cout, fmt, args...);
	}
	constexpr void FormatLn(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatInto(std::cout, fmt, args...);
		std::cout << '\n';
	}

	/* convenience for fast formatting to std::wcout */
	constexpr void WFormat(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatInto(std::wcout, fmt, args...);
	}
	constexpr void WFormatLn(const str::AnyString auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatInto(std::wcout, fmt, args...);
		std::wcout << L'\n';
	}

	/* convenience for fast building to std::cout */
	constexpr void Out(const str::IsFormattable auto&... args) {
		str::BuildInto(std::cout, args...);
	}
	constexpr void OutLn(const str::IsFormattable auto&... args) {
		str::BuildInto(std::cout, args...);
		std::cout << '\n';
	}

	/* convenience for fast building to std::wcout */
	constexpr void WOut(const str::IsFormattable auto&... args) {
		str::BuildInto(std::wcout, args...);
	}
	constexpr void WOutLn(const str::IsFormattable auto&... args) {
		str::BuildInto(std::wcout, args...);
		std::wcout << L'\n';
	}

	/*	Normal padding: in Order; all optional
	*	[char?[<^>]\??]: padding character and side
	*		=> [char]: char to be used for padding (default: ' ')
	*		=> [<^>]: alignment-side of content (default: trailing)
	*		=> [?]: replace un-encodable characters with '?' (default: none)
	*	[d+]: minimum-digits
	*	[[,;]d+]: maximum-digits (if ';': add ellipsis if content has been shortened) */
	namespace fmt {
		struct FmtNumber {
			size_t number = 0;
			size_t consumed = 0;
		};
		inline constexpr fmt::FmtNumber ParseIndicatedNumber(const std::u32string_view& fmt, char32_t indicator, size_t def) {
			fmt::FmtNumber out{};
			out.number = def;

			/* validate the first character */
			if (fmt.size() < 2 || fmt[0] != indicator)
				return out;

			/* parse the number (range-error will automatically result in the largest possible value) */
			auto [value, consumed, result] = str::ParseNum<size_t>(fmt.substr(1), 10, str::PrefixMode::none);
			if (result == str::NumResult::valid || result == str::NumResult::range) {
				out.consumed = consumed + 1;
				out.number = value;
			}
			return out;
		}

		enum class Alignment : uint8_t {
			standard,
			leading,
			trailing,
			center
		};

		/* parse default padding at the beginning of the formatting-string (if minimum/maximum is
		*	null, was not present; if maximum is set, it will always be greater/equal to minimum) */
		struct Padding {
			size_t minimum = 0;
			size_t maximum = 0;
			char32_t padChar = U' ';
			fmt::Alignment align = fmt::Alignment::standard;
			bool replaceError = false;
			bool ellipsisClipping = false;
		};
		inline constexpr size_t ParsePaddingAlignment(const std::u32string_view& fmt, fmt::Padding& out) {
			size_t consumed = 0;

			/* check if the format-string defines a padding mode */
			char32_t padModeChar = 0;
			if (fmt.size() > 1 && (fmt[1] == U'<' || fmt[1] == U'^' || fmt[1] == U'>')) {
				out.padChar = fmt[consumed++];
				padModeChar = fmt[consumed++];
			}
			else if (fmt.size() > 0 && (fmt[0] == U'<' || fmt[0] == U'^' || fmt[0] == U'>'))
				padModeChar = fmt[consumed++];
			if (padModeChar != 0)
				out.align = (padModeChar == U'>' ? fmt::Alignment::trailing : (padModeChar == U'^' ? fmt::Alignment::center : fmt::Alignment::trailing));

			/* parse the error-char */
			if (consumed < fmt.size() && fmt[consumed] == U'?') {
				out.replaceError = true;
				++consumed;
			}
			return consumed;
		}
		inline constexpr size_t ParsePaddingMinimum(const std::u32string_view& fmt, fmt::Padding& out) {
			/* parse the minimum length (range-error will automatically result in the largest possible value) */
			auto [value, consumed, result] = str::ParseNum<size_t>(fmt, 10, str::PrefixMode::none);
			if (result != str::NumResult::valid && result != str::NumResult::range)
				return 0;
			out.minimum = std::max<size_t>(1, value);
			return consumed;
		}
		inline constexpr size_t ParsePaddingMaximum(const std::u32string_view& fmt, fmt::Padding& out) {
			bool ellipsis = false;

			/* parse the number and check if ellipsis should be added */
			fmt::FmtNumber num = fmt::ParseIndicatedNumber(fmt, U',', 0);
			if (ellipsis = (num.consumed == 0))
				num = fmt::ParseIndicatedNumber(fmt, U';', 0);
			if (num.consumed == 0)
				return 0;

			/* apply the detected state */
			out.ellipsisClipping = ellipsis;
			out.maximum = std::max<size_t>({ 1, out.minimum, num.number });
			return num.consumed;
		}

		struct PadFormat {
			fmt::Padding padding;
			std::u32string_view rest;
		};
		inline constexpr fmt::PadFormat ParsePadding(const std::u32string_view& fmt) {
			fmt::PadFormat out{};
			size_t consumed = 0;

			/* parse the three components of the padding */
			consumed += fmt::ParsePaddingAlignment(fmt, out.padding);
			consumed += fmt::ParsePaddingMinimum(fmt.substr(consumed), out.padding);
			consumed += fmt::ParsePaddingMaximum(fmt.substr(consumed), out.padding);

			out.rest = fmt.substr(consumed);
			return out;
		}

		/* write the padded string out and apply the corresponding padding */
		constexpr void WritePadded(str::AnySink auto&& sink, const std::u32string_view& str, const fmt::Padding& padding) {
			char32_t cpError = (padding.replaceError ? cp::DefErrorChar : 0);

			/* check if the string is smaller than the minimum and add the padding */
			if (str.size() < padding.minimum) {
				size_t diff = (padding.minimum - str.size());

				/* add the leading padding */
				if (padding.align == fmt::Alignment::trailing || padding.align == fmt::Alignment::standard)
					str::AppChars(sink, padding.padChar, diff, cpError);
				else if (padding.align == fmt::Alignment::center)
					str::AppChars(sink, padding.padChar, diff / 2, cpError);

				/* add the string itself */
				str::Append(sink, str, cpError);

				/* add the trailing padding */
				if (padding.align == fmt::Alignment::leading)
					str::AppChars(sink, padding.padChar, diff, cpError);
				else if (padding.align == fmt::Alignment::center)
					str::AppChars(sink, padding.padChar, diff - (diff / 2), cpError);
				return;
			}

			/* check if the string needs to be clipped or can just be written out */
			if (padding.maximum == 0 || str.size() <= padding.maximum)
				str::Append(sink, str, cpError);
			else if (!padding.ellipsisClipping)
				str::Append(sink, str.substr(0, padding.maximum), cpError);
			else if (padding.maximum > 3) {
				str::Append(sink, str.substr(0, padding.maximum - 3), cpError);
				str::Append(sink, U"...", cpError);
			}
			else
				str::Append(sink, std::u32string_view(U"...", padding.maximum), cpError);
		}
	}

	namespace detail {
		struct NumPreamble {
			size_t consumed = 0;
			char32_t signChar = U'-';
			bool prefix = false;
			bool nullPadding = false;
		};
		inline constexpr detail::NumPreamble ParseNumPreamble(const std::u32string_view& fmt, bool canNullPad) {
			detail::NumPreamble out;

			/* validate the intermediate characters and parse them */
			if (fmt.size() > 0 && (fmt[0] == U'-' || fmt[0] == U' ' || fmt[0] == U'+')) {
				out.signChar = fmt[0];
				++out.consumed;
			}
			if (out.consumed < fmt.size() && fmt[out.consumed] == U'#') {
				out.prefix = true;
				++out.consumed;
			}

			/* check if the string should be null-padded (only if the alignment has not been overwritten,
			*	otherwise simply ignore the 0, as it can just be interpreted as being from the minimum size) */
			if (out.consumed < fmt.size() && fmt[out.consumed] == U'0') {
				if (canNullPad)
					out.nullPadding = true;
				++out.consumed;
			}
			return out;
		}

		struct NumRadix {
			size_t radix = 10;
			bool upperCase = false;
			bool found = false;
		};
		inline constexpr detail::NumRadix ParseNumRadix(const std::u32string_view& fmt, bool allowHexFloat) {
			if (fmt.empty())
				return detail::NumRadix();
			detail::NumRadix out;

			/* check what radix it is */
			if (fmt[0] == U'b' || fmt[0] == U'B') {
				out.upperCase = (fmt[0] == U'B');
				out.radix = 2;
			}
			else if (fmt[0] == U'q' || fmt[0] == U'Q') {
				out.upperCase = (fmt[0] == U'Q');
				out.radix = 4;
			}
			else if (fmt[0] == U'o' || fmt[0] == U'O') {
				out.upperCase = (fmt[0] == U'O');
				out.radix = 8;
			}
			else if (fmt[0] == U'd' || fmt[0] == U'D') {
				out.upperCase = (fmt[0] == U'D');
				out.radix = 10;
			}
			else if (fmt[0] == U'x' || fmt[0] == U'X') {
				out.upperCase = (fmt[0] == U'X');
				out.radix = 16;
			}
			else if (allowHexFloat && (fmt[0] == U'a' || fmt[0] == U'A')) {
				out.upperCase = (fmt[0] == U'A');
				out.radix = str::HexFloat;
			}
			else
				return detail::NumRadix();

			/* mark it as found and consumed */
			out.found = true;
			return out;
		}

		template <class Type>
		constexpr void NumPreambleInto(auto& sink, Type& val, char32_t signChar, size_t radix, bool upperCase, bool prefix) {
			/* check if a sign-character needs to be added */
			if (val < 0) {
				if constexpr (std::is_signed_v<Type>)
					val = -val;
				str::AppChars(sink, U'-');
			}
			else if (signChar != U'-' && signChar != 0)
				str::AppChars(sink, signChar);

			/* check if a prefix needs to be added */
			if (prefix)
				str::Append(sink, str::MakePrefix<char32_t>(radix, upperCase));
		}

		struct StrFormatting {
			bool ascii = false;
			bool escape = false;
		};
		constexpr detail::StrFormatting ParseStrFormatting(const std::u32string_view& fmt) {
			detail::StrFormatting out{};

			/* check if the string contains the aA or eE characters */
			if (!fmt.empty() && (fmt[0] == U'a' || fmt[0] == U'A' || fmt[0] == U'e' || fmt[0] == U'E')) {
				out.ascii = (fmt[0] == U'a' || fmt[0] == U'A');
				out.escape = true;
			}
			return out;
		}

		template <class Type>
		constexpr bool FormatChar(auto& sink, Type val, const std::u32string_view& fmt) {
			/* parse the padding format */
			auto [padding, rest] = fmt::ParsePadding(fmt);
			char32_t cpError = (padding.replaceError ? cp::DefErrorChar : 0);

			/* parse the count */
			auto [count, _consumed] = fmt::ParseIndicatedNumber(rest, U'@', 1);
			rest = rest.substr(_consumed);

			/* parse the string-formatting */
			auto [ascii, escape] = detail::ParseStrFormatting(rest);
			if (escape)
				rest = rest.substr(1);

			/* check if the entire string has been consumed */
			if (!rest.empty())
				return false;

			/* check if the character can just be added */
			if (!escape && padding.minimum <= count && padding.maximum == 0) {
				str::AppChars(sink, val, count, cpError);
				return true;
			}

			/* decode the character to a codepoint */
			auto [cp, _] = str::Decode(std::basic_string_view<Type>{ &val, 1 }, true);
			if (!cp::Valid(cp)) {
				if (!padding.replaceError)
					return true;
				cp = cp::DefErrorChar;
			}

			/* create the temporary buffer containing the single codepoint */
			str::U32Small<10> buffer;
			if (escape && (ascii || cp::IsAscii(cp)))
				str::EscapeAsciiInto(buffer, cp, true);
			else
				buffer.push_back(cp);

			/* check if the codepoints themselves need to be written out */
			if (padding.minimum <= buffer.size() * count && (padding.maximum == 0 || padding.maximum >= buffer.size() * count)) {
				for (size_t i = 0; i < count; ++i)
					str::Append(sink, buffer, cpError);
				return true;
			}

			/* create the temporary buffer and let the writer handle it */
			std::u32string bufTotal;
			for (size_t i = 0; i < count; ++i)
				str::Append(bufTotal, buffer, cpError);
			fmt::WritePadded(sink, bufTotal, padding);
			return true;
		}
	}

	/*	Normal padding but:
	*	=> inbetween alignment and minimum-digits:
	*		[-+ ]: character for sign-place
	*		[#]: add prefix
	*		[0]: Optional (if no alignment specified, to indicate null-padding)
	*	[bBqQoOdDxX]: radix and casing */
	template <str::IsInteger Type> struct Formatter<Type> {
		constexpr bool operator()(str::AnySink auto& sink, Type val, const std::u32string_view& fmt) const {
			fmt::Padding padding{};
			size_t consumed = 0;

			/* parse the initial padding alignment */
			consumed += fmt::ParsePaddingAlignment(fmt, padding);

			/* parse the number-preamble requirements (only allow null-padding if no alignment has been specified) */
			auto [_consumed0, signChar, addPrefix, useNullPadding] = detail::ParseNumPreamble(fmt.substr(consumed), padding.align == fmt::Alignment::standard);
			consumed += _consumed0;

			/* parse the minimum and maximum padding state */
			consumed += fmt::ParsePaddingMinimum(fmt.substr(consumed), padding);
			consumed += fmt::ParsePaddingMaximum(fmt.substr(consumed), padding);

			/* parse the num-radix */
			auto [radix, upperCase, _radixFound] = detail::ParseNumRadix(fmt.substr(consumed), false);
			if (_radixFound)
				++consumed;

			/* check if the entire format has been consumed */
			if (consumed < fmt.size())
				return false;

			/* check if the number can just be written out */
			if (padding.minimum <= 1 && padding.maximum == 0) {
				detail::NumPreambleInto<Type>(sink, val, signChar, radix, upperCase, addPrefix);
				str::IntInto(sink, val, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));
				return true;
			}

			/* check if the number is to be null-padded */
			if (useNullPadding) {
				str::U32Small<4> prefix;
				detail::NumPreambleInto<Type>(prefix, val, signChar, radix, upperCase, addPrefix);

				/* write the preamble to the sink */
				if (padding.maximum == 0)
					str::Append(sink, prefix);
				else {
					str::Append(sink, prefix.view().substr(padding.maximum));
					if (padding.maximum >= prefix.size())
						return true;
					padding.maximum -= prefix.size();
				}

				/* write the integer to an intermediate buffer to estimate its size */
				std::u32string buffer;
				str::IntInto(buffer, val, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));

				/* check if the buffer must be clipped */
				if (padding.maximum != 0 && buffer.size() >= padding.maximum) {
					str::Append(sink, buffer.substr(padding.maximum));
					return true;
				}

				/* write the nulls to the sink */
				if (prefix.size() + buffer.size() < padding.minimum)
					str::AppChars(sink, U'0', padding.minimum - prefix.size() - buffer.size());

				/* write the integer itself to the sink */
				str::Append(sink, buffer);
				return true;
			}

			/* write the number to an intermediate buffer */
			std::u32string buffer;
			detail::NumPreambleInto<Type>(buffer, val, signChar, radix, upperCase, addPrefix);
			str::IntInto(buffer, val, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding but:
	*	=> inbetween alignment and minimum-digits:
	*		[-+ ]: character for sign-place
	*		[#]: add prefix
	*		[0]: Optional (if no alignment specified, to indicate null-padding)
	*	=> inbetween minimum-digits and maximum-digits:
	*		[.d+]: precision (default: 0)
	*	[bBqQoOdDxXaA]: radix and casing (aA: hex-float)
	*	[eEgGfF]: style
	*		=> eE: FloatStyle::scientific
	*		=> gG: FloatStyle::general
	*		=> fF: FloatStyle::fixed */
	template <str::IsFloat Type> struct Formatter<Type> {
		constexpr bool operator()(str::AnySink auto& sink, Type val, const std::u32string_view& fmt) const {
			fmt::Padding padding{};
			size_t consumed = 0;

			/* parse the initial padding alignment */
			consumed += fmt::ParsePaddingAlignment(fmt, padding);

			/* parse the number-preamble requirements (only allow null-padding if no alignment has been specified) */
			auto [_consumed0, signChar, addPrefix, useNullPadding] = detail::ParseNumPreamble(fmt.substr(consumed), padding.align == fmt::Alignment::standard);
			consumed += _consumed0;

			/* parse the minimum padding state */
			consumed += fmt::ParsePaddingMinimum(fmt.substr(consumed), padding);

			/* parse the precision between the minimum and maximum */
			auto [precision, _consumed1] = fmt::ParseIndicatedNumber(fmt.substr(consumed), U'.', 0);
			consumed += _consumed1;

			/* parse the maximum padding state */
			consumed += fmt::ParsePaddingMaximum(fmt.substr(consumed), padding);

			/* parse the num-radix */
			auto [radix, upperCase, _radixFound] = detail::ParseNumRadix(fmt.substr(consumed), false);
			if (_radixFound)
				++consumed;

			/* parse the float-style */
			str::FloatStyle style = str::FloatStyle::general;
			if (consumed < fmt.size()) {
				if (fmt[consumed] == U'f' || fmt[consumed] == U'F') {
					style = str::FloatStyle::fixed;
					++consumed;
				}
				else if (fmt[consumed] == U'g' || fmt[consumed] == U'G') {
					style = str::FloatStyle::general;
					++consumed;
				}
				else if (fmt[consumed] == U'e' || fmt[consumed] == U'E') {
					style = str::FloatStyle::scientific;
					++consumed;
				}
			}

			/* check if the entire format has been consumed */
			if (consumed < fmt.size())
				return false;

			/* check if the number can just be written out */
			if (padding.minimum <= 1 && padding.maximum == 0) {
				detail::NumPreambleInto<Type>(sink, val, signChar, radix, upperCase, addPrefix);
				str::FloatInto(sink, val, style, precision, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));
				return true;
			}

			/* check if the number is to be null-padded */
			if (useNullPadding) {
				str::U32Small<4> prefix;
				detail::NumPreambleInto(prefix, val, signChar, radix, upperCase, addPrefix);

				/* write the preamble to the sink */
				if (padding.maximum == 0)
					str::Append(sink, prefix);
				else {
					str::Append(sink, prefix.view().substr(padding.maximum));
					if (padding.maximum >= prefix.size())
						return true;
					padding.maximum -= prefix.size();
				}

				/* write the float to an intermediate buffer to estimate its size */
				std::u32string buffer;
				str::FloatInto(buffer, val, style, precision, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));

				/* check if the buffer must be clipped */
				if (padding.maximum != 0 && buffer.size() >= padding.maximum) {
					str::Append(sink, buffer.substr(padding.maximum));
					return true;
				}

				/* write the nulls to the sink */
				if (prefix.size() + buffer.size() < padding.minimum)
					str::AppChars(sink, U'0', padding.minimum - prefix.size() - buffer.size());

				/* write the float itself to the sink */
				str::Append(sink, buffer);
				return true;
			}

			/* write the number to an intermediate buffer */
			std::u32string buffer;
			detail::NumPreambleInto(buffer, val, signChar, radix, upperCase, addPrefix);
			str::FloatInto(buffer, val, style, precision, radix, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[eE]: escape: 0x00-0x1f;0x7f as \x.. and common escape sequences
	*	[aA]: ascii: escape and write 0x80-... as \u{....} (if not in escape-mode) */
	template <str::AnyString Type> struct Formatter<Type> {
		constexpr bool operator()(str::AnySink auto& sink, const Type& t, const std::u32string_view& fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);
			char32_t cpError = (padding.replaceError ? cp::DefErrorChar : 0);

			/* parse the string-formatting */
			auto [ascii, escape] = detail::ParseStrFormatting(rest);
			if (escape)
				rest = rest.substr(1);

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* check if the string can just be appended */
			if (!escape && padding.minimum <= 1 && padding.maximum == 0) {
				str::Append(sink, t, cpError);
				return true;
			}

			/* write the string to an intermediate buffer */
			std::u32string buffer;
			if (escape) {
				using ChType = str::StringCharType<Type>;
				std::basic_string_view<ChType> view = str::StringView<ChType>(t);

				/* extract all separate characters */
				while (!view.empty()) {
					auto [cp, consumed] = str::Decode(view, true);
					view = view.substr(consumed);

					/* create the escape sequence or add the error-codepoint if the codepoint could not be decoded */
					if (cp::Valid(cp)) {
						if (ascii || cp::IsAscii(cp))
							str::EscapeAsciiInto(buffer, cp, true);
						else
							buffer.push_back(cp);
					}
					else if (padding.replaceError)
						buffer.push_back(cp::DefErrorChar);
				}
			}
			else
				str::Append(buffer, t, cpError);

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[uU]: uppercase */
	template <> struct Formatter<void*> {
		constexpr bool operator()(str::AnySink auto& sink, void* val, const std::u32string_view& fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the final arguments and validate them */
			bool upperCase = false;
			if (!rest.empty() && (rest[0] == U'u' || rest[0] == U'U')) {
				upperCase = true;
				rest = rest.substr(1);
			}

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* construct the output-string */
			str::U32Small<sizeof(void*) * 2> buffer;
			for (size_t i = sizeof(void*) * 2; i > 0 && (uintptr_t(val) >> (i - 1) * 4) == 0; --i)
				buffer.push_back(U'0');
			str::IntInto(buffer, uintptr_t(val), 16, (upperCase ? str::NumStyle::upper : str::NumStyle::lower));

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[sS]: as string (true/false vs True/False) */
	template <> struct Formatter<bool> {
		constexpr bool operator()(str::AnySink auto& sink, bool val, const std::u32string_view& fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the final arguments and validate them */
			std::u32string_view str = (val ? U"1" : U"0");
			if (!rest.empty()) {
				if (rest[0] == U's') {
					str = (val ? U"true" : U"false");
					rest = rest.substr(1);
				}
				else if (rest[0] == U'S') {
					str = (val ? U"True" : U"False");
					rest = rest.substr(1);
				}
			}

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* write the padded string to the sink */
			fmt::WritePadded(sink, str, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[@d+]: number of times to repeat char (default: 1)
	*	[eE]: escape: 0x00-0x1f;0x7f as \x.. and common escape sequences
	*	[aA]: ascii: escape and write 0x80-... as \u{....} (if not in escape-mode) */
	template <> struct Formatter<char> {
		constexpr bool operator()(str::AnySink auto& sink, char val, const std::u32string_view& fmt) const {
			return detail::FormatChar<char>(sink, val, fmt);
		}
	};
	template <> struct Formatter<wchar_t> {
		constexpr bool operator()(str::AnySink auto& sink, wchar_t val, const std::u32string_view& fmt) const {
			return detail::FormatChar<wchar_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char8_t> {
		constexpr bool operator()(str::AnySink auto& sink, char8_t val, const std::u32string_view& fmt) const {
			return detail::FormatChar<char8_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char16_t> {
		constexpr bool operator()(str::AnySink auto& sink, char16_t val, const std::u32string_view& fmt) const {
			return detail::FormatChar<char16_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char32_t> {
		constexpr bool operator()(str::AnySink auto& sink, char32_t val, const std::u32string_view& fmt) const {
			return detail::FormatChar<char32_t>(sink, val, fmt);
		}
	};
}
