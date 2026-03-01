/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024-2026 Bjoern Boss Henrichsen */
#pragma once

#include "../unicode/cp-property.h"
#include "../coding/str-coding.h"
#include "str-number.h"
#include "str-sivalue.h"
#include "str-escape.h"

#include <filesystem>

/*
*	Coding-Rules:
*	 - decoding using str::Ascii/str::GetCodepoint<str::CodeError::nothing> for format-string itself (any invalid codepoints will result in an #malformed error in the format-string)
*	 - decoding using str::GetCodepoint<str::CodeError::replace> for character/string as formattable arguments
*	 - encoding using str::CodepointTo<str::CodeError::replace>/str::FastcodeAllTo<str::CodeError::replace> for all printable output
*		(except when produced by other functions using other rules, such as str::Int/...)
*/
namespace str {
	/* formattable interface which requires:
	*	operator() to take a sink of any type, a value of the type, a utf32 string-view with formatting-string and a boolean return-value
	*	if the format-string was valid (should leave the sink untouched if the format is invalid; empty format should be valid at all times) */
	template <class Type>
	struct Formatter;

	/* type is anything that implements thr str::Formatter interface */
	template <class Type>
	concept IsFormattable = requires(const Type & val, const std::u32string_view & fmt, std::string & cs, std::wstring & ws, std::u8string & u8s, std::u16string & u16s, std::u32string & u32s) {
		{ str::Formatter<std::remove_cvref_t<Type>>{}(cs, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(ws, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u8s, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u16s, val, fmt) } -> std::same_as<bool>;
		{ str::Formatter<std::remove_cvref_t<Type>>{}(u32s, val, fmt) } -> std::same_as<bool>;
	};

	/* wrapper to interact with formatters */
	constexpr bool CallFormat(str::IsSink auto&& sink, const str::IsFormattable auto& val, std::u32string_view fmt = {}) {
		return str::Formatter<std::remove_cvref_t<decltype(val)>>{}(sink, val, fmt);
	}

	namespace detail {
		template <class ChType>
		constexpr int8_t FormatIndex(auto& sink, size_t index, std::u32string_view fmt) {
			return -1;
		}

		template <class ChType, class Arg, class... Args>
		constexpr int8_t FormatIndex(auto& sink, size_t index, std::u32string_view fmt, const Arg& arg, const Args&... args) {
			if (index > 0)
				return detail::FormatIndex<ChType, Args...>(sink, index - 1, fmt, args...);
			else
				return (str::CallFormat(sink, arg, fmt) ? 1 : 0);
		}

		template <class ChType, class Arg, class... Args>
		constexpr void Append(auto& sink, const Arg& arg, const Args&... args) {
			if (!str::CallFormat(sink, arg, U""))
				str::FastcodeAllTo<str::CodeError::replace>(sink, U"#fmt");
			if constexpr (sizeof...(args) > 0)
				detail::Append<ChType, Args...>(sink, args...);
		}

		template <class FmtType, class SinkType>
		constexpr bool FormatPrintUntilArg(auto& sink, std::basic_string_view<FmtType>& fmt) {
			/* iterate until the entire format-string has been processed or until an argument has been encountered */
			bool openStarted = false;
			while (!fmt.empty()) {
				/* decode the next character */
				auto [cp, len] = str::GetCodepoint<str::CodeError::nothing>(fmt);
				if (cp == str::Invalid)
					return false;

				/* check if an open-bracket has been encountered, which could either be part of an escape sequence or mark the start of an argument */
				if (cp == U'{')
					openStarted = !openStarted;

				/* check if an opening bracket was the last token, in which case this must have been a valid argument start */
				else if (openStarted)
					return true;

				/* check if the token should be committed to the sink */
				if (!openStarted) {
					if constexpr (str::EffSame<FmtType, SinkType>)
						str::CallSink(sink, std::basic_string_view<SinkType>{ reinterpret_cast<const SinkType*>(fmt.data()), len });
					else
						str::CodepointTo<str::CodeError::replace>(sink, cp, 1);
				}
				fmt = fmt.substr(len);
			}
			return true;
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
				auto [cp, len] = str::GetAscii<str::CodeError::nothing>(fmt);
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
	void FormatTo(str::IsSink auto&& sink, const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		using FmtType = str::StringChar<decltype(fmt)>;
		using SinkType = str::SinkChar<decltype(sink)>;
		enum class ArgValid : uint8_t {
			valid,
			malformed,
			index,
			format
		};

		/* buffer the format-string until the entire format has been consumed */
		size_t argSequence = 0;
		std::u32string argFormatBuffer;
		std::basic_string_view<FmtType> view{ fmt };

		while (true) {
			size_t argIndex = 0;

			/* print all characters from the format string until the next argument starts and check if an error occurred */
			ArgValid fmtState = ArgValid::valid;
			if (!detail::FormatPrintUntilArg<FmtType, SinkType>(sink, view))
				fmtState = ArgValid::malformed;
			else if (view.empty())
				break;
			else {
				argFormatBuffer.clear();

				/* parse the argument-index (ignore range errors as they will just result in the largest possible value) */
				auto [value, consumed, result] = str::ParseNum<size_t>(view);
				if (result == str::NumResult::valid || result == str::NumResult::range) {
					view = view.substr(consumed);
					argIndex = value;
				}
				else
					argIndex = argSequence++;

				/* iterate over the remaining characters until the end of the argument has been reached */
				bool hasSeparator = false;
				while (true) {
					auto [cp, len] = str::GetCodepoint<str::CodeError::nothing>(view);
					if (cp == str::Invalid) {
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
			}

			/* check if a well-formed argument has been found and print it */
			if (fmtState == ArgValid::valid) {
				int8_t res = detail::FormatIndex<SinkType>(sink, argIndex, argFormatBuffer, args...);
				if (res != 1)
					fmtState = (res < 0 ? ArgValid::index : ArgValid::format);
			}

			/* check if a format-string issue was encountered */
			if (fmtState == ArgValid::index)
				str::FastcodeAllTo<str::CodeError::replace>(sink, U"#index");
			else if (fmtState == ArgValid::format)
				str::FastcodeAllTo<str::CodeError::replace>(sink, U"#fmt");

			/* check if the entire format-string was malformed, in which case further arguments will not be processed */
			else if (fmtState == ArgValid::malformed) {
				str::FastcodeAllTo<str::CodeError::replace>(sink, U"#malformed");
				break;
			}
		}
	}

	/* format the arguments to an object of the given sink-type using str::FormatTo and return it */
	template <str::IsSink SinkType>
	constexpr SinkType Format(const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		SinkType sink{};
		str::FormatTo(sink, fmt, args...);
		return sink;
	}

	/* build the arguments into the sink (as if formatting with format "{}{}{}...") */
	constexpr void BuildTo(str::IsSink auto&& sink, const str::IsFormattable auto&... args) {
		using ChType = str::SinkChar<decltype(sink)>;
		if constexpr (sizeof...(args) > 0)
			detail::Append<ChType>(sink, args...);
	}

	/* build the arguments to an object of the given sink-type using str::BuildTo and return it */
	template <str::IsSink SinkType>
	constexpr SinkType Build(const str::IsFormattable auto&... args) {
		SinkType sink{};
		str::BuildTo(sink, args...);
		return sink;
	}

	/* format a value using si-units */
	template <str::IsNumber NumType, str::IsStr UnitType>
	struct Si {
	public:
		std::basic_string_view<str::StringChar<UnitType>> unit;
		NumType value = 0;

	public:
		constexpr Si(NumType value, const UnitType& unit) : value{ value }, unit{ unit } {}
	};

	/* bind the given value to be formatted using the given formatting-string, which is useful to format build-output (no own formatting)
	*	Note: Must not outlive the value object as it stores a reference to it */
	template <str::IsStr FmtType, str::IsFormattable Type>
	struct As {
	public:
		std::basic_string_view<str::StringChar<FmtType>> format;
		const Type& value;

	public:
		constexpr As(const FmtType& fmt, const Type& value) : format{ fmt }, value{ value } {}
	};

	/* type is anything that implements an forward iterator on formattable types */
	template <class Type>
	concept IsFmtRangeIt = std::forward_iterator<Type> && str::IsFormattable<typename std::iterator_traits<Type>::value_type>;

	/* type is anything that implements a begin() and end() and const_iterator type, which implements str::IsFmtRangeIt */
	template <class Type>
	concept IsFmtRange = requires(const Type & val) {
		typename Type::const_iterator;
		{ val.begin() } -> std::same_as<typename Type::const_iterator>;
		{ val.end() } -> std::same_as<typename Type::const_iterator>;
			requires str::IsFmtRangeIt<typename Type::const_iterator>;
	};

	/* format a range of values defined by iterators */
	template <str::IsFmtRangeIt ItType>
	struct Range {
	public:
		ItType begin;
		ItType end;

	public:
		constexpr Range(ItType begin, ItType end) : begin{ begin }, end{ end } {}
		constexpr Range(const str::IsFmtRange auto& val) : begin{ val.begin() }, end{ val.end() } {}
	};
	template <class Type>
	Range(const Type&) -> Range<typename Type::const_iterator>;

	/* add a formatted value multiple times */
	template <str::IsFormattable Type>
	struct Repeat {
	public:
		const Type& value;
		size_t count = 0;

	public:
		constexpr Repeat(const Type& value, size_t count = 1) : value{ value }, count{ count } {}
	};

	/*	Normal padding: in Order; all optional
	*	[char?[<^>]]: padding character and side
	*		=> [char]: char to be used for padding (default: ' ')
	*		=> [<^>]: alignment-side of content (default: trailing)
	*	[d+]: minimum-digits
	*	[[,;]d+]: maximum-digits (if ';': add ellipsis if content has been shortened) */
	namespace fmt {
		struct FmtNumber {
			size_t number = 0;
			size_t consumed = 0;
		};
		inline constexpr fmt::FmtNumber ParseIndicatedNumber(std::u32string_view fmt, char32_t indicator, size_t def) {
			fmt::FmtNumber out{};
			out.number = def;

			/* validate the first character */
			if (fmt.size() < 2 || fmt[0] != indicator)
				return out;

			/* parse the number (range-error will automatically result in the largest possible value) */
			auto [value, consumed, result] = str::ParseNum<size_t>(fmt.substr(1), { .radix = 10, .prefix = str::PrefixMode::none });
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
			bool ellipsisClipping = false;
		};
		inline constexpr size_t ParsePaddingAlignment(std::u32string_view fmt, fmt::Padding& out) {
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
				out.align = (padModeChar == U'>' ? fmt::Alignment::trailing : (padModeChar == U'^' ? fmt::Alignment::center : fmt::Alignment::leading));
			return consumed;
		}
		inline constexpr size_t ParsePaddingMinimum(std::u32string_view fmt, fmt::Padding& out) {
			/* parse the minimum length (range-error will automatically result in the largest possible value) */
			auto [value, consumed, result] = str::ParseNum<size_t>(fmt, { .radix = 10, .prefix = str::PrefixMode::none });
			if (result != str::NumResult::valid && result != str::NumResult::range)
				return 0;
			out.minimum = std::max<size_t>(1, value);
			return consumed;
		}
		inline constexpr size_t ParsePaddingMaximum(std::u32string_view fmt, fmt::Padding& out) {
			bool ellipsis = false;

			/* parse the number and check if ellipsis should be added */
			fmt::FmtNumber num = fmt::ParseIndicatedNumber(fmt, U',', 0);
			if ((ellipsis = (num.consumed == 0)))
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
		inline constexpr fmt::PadFormat ParsePadding(std::u32string_view fmt) {
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
		constexpr void WritePadded(str::IsSink auto&& sink, std::u32string_view str, const fmt::Padding& padding) {
			/* check if the string is smaller than the minimum and add the padding */
			if (str.size() < padding.minimum) {
				size_t diff = (padding.minimum - str.size());

				/* add the leading padding */
				if (padding.align == fmt::Alignment::trailing || padding.align == fmt::Alignment::standard)
					str::CodepointTo<str::CodeError::replace>(sink, padding.padChar, diff);
				else if (padding.align == fmt::Alignment::center)
					str::CodepointTo<str::CodeError::replace>(sink, padding.padChar, diff / 2);

				/* add the string itself */
				str::FastcodeAllTo<str::CodeError::replace>(sink, str);

				/* add the trailing padding */
				if (padding.align == fmt::Alignment::leading)
					str::CodepointTo<str::CodeError::replace>(sink, padding.padChar, diff);
				else if (padding.align == fmt::Alignment::center)
					str::CodepointTo<str::CodeError::replace>(sink, padding.padChar, diff - (diff / 2));
				return;
			}

			/* check if the string needs to be clipped or can just be written out */
			if (padding.maximum == 0 || str.size() <= padding.maximum)
				str::FastcodeAllTo<str::CodeError::replace>(sink, str);
			else if (!padding.ellipsisClipping)
				str::FastcodeAllTo<str::CodeError::replace>(sink, str.substr(0, padding.maximum));
			else if (padding.maximum > 3) {
				str::FastcodeAllTo<str::CodeError::replace>(sink, str.substr(0, padding.maximum - 3));
				str::FastcodeAllTo<str::CodeError::replace>(sink, U"...");
			}
			else
				str::FastcodeAllTo<str::CodeError::replace>(sink, std::u32string_view(U"...", padding.maximum));
		}
	}

	namespace detail {
		struct NumPreamble {
			size_t consumed = 0;
			char32_t signChar = U'-';
			bool prefix = false;
			bool nullPadding = false;
		};
		inline constexpr detail::NumPreamble ParseNumPreamble(std::u32string_view fmt, bool canNullPad) {
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
		inline constexpr detail::NumRadix ParseNumRadix(std::u32string_view fmt, bool allowHexFloat, bool allowSigned, bool allowUnsigned) {
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
			else if (allowSigned && (fmt[0] == U's' || fmt[0] == U'S')) {
				out.upperCase = (fmt[0] == U'S');
				out.radix = 10;
			}
			else if (allowUnsigned && (fmt[0] == U'u' || fmt[0] == U'U')) {
				out.upperCase = (fmt[0] == U'U');
				out.radix = 10;
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
				str::CodepointTo<str::CodeError::replace>(sink, U'-');
			}
			else if (signChar != U'-' && signChar != 0)
				str::CodepointTo<str::CodeError::replace>(sink, signChar);

			/* check if a prefix needs to be added */
			if (prefix)
				str::FastcodeAllTo<str::CodeError::replace>(sink, str::MakePrefix(radix, upperCase));
		}

		struct StrFormatting {
			bool ascii = false;
			bool escape = false;
		};
		constexpr detail::StrFormatting ParseStrFormatting(std::u32string_view fmt) {
			detail::StrFormatting out{};

			/* check if the string contains the aA or eE characters */
			if (!fmt.empty() && (fmt[0] == U'a' || fmt[0] == U'e')) {
				out.ascii = (fmt[0] == U'a');
				out.escape = true;
			}
			return out;
		}

		template <class Type>
		bool FormatChar(auto& sink, Type val, std::u32string_view fmt) {
			/* parse the padding format */
			auto [padding, rest] = fmt::ParsePadding(fmt);

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

			/* decode the character to a codepoint */
			auto [cp, _] = str::GetCodepoint<str::CodeError::replace>(std::basic_string_view<Type>{ &val, 1 });
			if (cp == str::Invalid)
				return true;

			/* check if the character can just be added */
			if (!escape && padding.minimum <= count && padding.maximum == 0) {
				str::CodepointTo<str::CodeError::replace>(sink, cp, count);
				return true;
			}

			/* create the temporary buffer containing the single codepoint */
			str::Local<char32_t, str::MaxEscapeSize> buffer;
			if (escape && (ascii || !cp::prop::IsPrint(cp)))
				str::EscapeTo(buffer, cp, true);
			else
				buffer.push_back(cp);

			/* check if the codepoints themselves need to be written out */
			if (padding.minimum <= buffer.size() * count && (padding.maximum == 0 || padding.maximum >= buffer.size() * count)) {
				for (size_t i = 0; i < count; ++i)
					str::FastcodeAllTo<str::CodeError::replace>(sink, buffer);
				return true;
			}

			/* create the temporary buffer and let the writer handle it */
			std::u32string bufTotal;
			for (size_t i = 0; i < count; ++i)
				str::FastcodeAllTo<str::CodeError::replace>(bufTotal, buffer);
			fmt::WritePadded(sink, bufTotal, padding);
			return true;
		}

		struct FltFormat {
			size_t consumed = 0;
			fmt::Padding padding;
			size_t radix = 10;
			size_t precision = 0;
			char32_t signChar = U'-';
			str::FloatStyle style = str::FloatStyle::general;
			bool prefix = false;
			bool nullPadding = false;
			bool upperCase = false;
		};
		constexpr detail::FltFormat ParseFltFormatting(std::u32string_view fmt) {
			detail::FltFormat flt;

			/* parse the initial padding alignment */
			flt.consumed += fmt::ParsePaddingAlignment(fmt, flt.padding);

			/* parse the number-preamble requirements (only allow null-padding if no alignment has been specified) */
			auto [_consumed0, signChar, prefix, nullPadding] = detail::ParseNumPreamble(fmt.substr(flt.consumed), flt.padding.align == fmt::Alignment::standard);
			flt.consumed += _consumed0;
			flt.signChar = signChar;
			flt.prefix = prefix;
			flt.nullPadding = nullPadding;

			/* parse the minimum padding state */
			flt.consumed += fmt::ParsePaddingMinimum(fmt.substr(flt.consumed), flt.padding);

			/* parse the precision between the minimum and maximum */
			auto [precision, _consumed1] = fmt::ParseIndicatedNumber(fmt.substr(flt.consumed), U'.', 0);
			flt.consumed += _consumed1;
			flt.precision = precision;

			/* parse the maximum padding state */
			flt.consumed += fmt::ParsePaddingMaximum(fmt.substr(flt.consumed), flt.padding);

			/* parse the num-radix */
			auto [radix, upperCase, _radixFound] = detail::ParseNumRadix(fmt.substr(flt.consumed), true, false, false);
			if (_radixFound)
				++flt.consumed;
			flt.radix = radix;
			flt.upperCase = upperCase;

			/* parse the float-style */
			if (flt.consumed >= fmt.size())
				return flt;

			/* match the separate types */
			if (fmt[flt.consumed] == U'f') {
				flt.style = str::FloatStyle::fixed;
				++flt.consumed;
			}
			else if (fmt[flt.consumed] == U'g') {
				flt.style = str::FloatStyle::general;
				++flt.consumed;
			}
			else if (fmt[flt.consumed] == U'e') {
				flt.style = str::FloatStyle::scientific;
				++flt.consumed;
			}
			return flt;
		}

		template <class Type>
		std::u32string WriteFltBuffered(const detail::FltFormat& fmt, Type val, const str::ArgsFloat& floatArgs) {
			std::u32string buffer;

			/* write the prefix to be used to the buffer */
			detail::NumPreambleInto<Type>(buffer, val, fmt.signChar, fmt.radix, fmt.upperCase, fmt.prefix);

			/* check if the number can just be written out */
			if (!fmt.nullPadding) {
				str::FloatTo(buffer, val, floatArgs);
				return buffer;
			}

			/* write the number to an intermediate buffer to estimate its size */
			std::u32string temp;
			str::FloatTo(temp, val, floatArgs);

			/* write the nulls to the buffer and write the number itself to the buffer */
			if (buffer.size() + temp.size() < fmt.padding.minimum)
				str::CodepointTo<str::CodeError::replace>(buffer, U'0', fmt.padding.minimum - buffer.size() - temp.size());
			str::FastcodeAllTo<str::CodeError::replace>(buffer, temp);
			return buffer;
		}

		constexpr void SiEpilogueInto(auto& sink, const str::Local<char32_t, 2>& prefix, const auto& unit, bool space, bool always, bool two) {
			/* write the space out */
			if (space)
				str::CodepointTo<str::CodeError::replace>(sink, U' ', 1);

			/* write the prefix out and unit out */
			if (!prefix.empty())
				str::FastcodeAllTo<str::CodeError::replace>(sink, prefix);
			str::FastcodeAllTo<str::CodeError::replace>(sink, unit);

			/* check if spaces should be appended */
			if (prefix.empty() && always)
				str::CodepointTo<str::CodeError::replace>(sink, U' ', (two ? 2 : 1));
		}

		inline std::pair<std::u32string_view, std::u32string_view> ParseElementFormatting(std::u32string_view fmt, std::u32string& _buffer) {
			/* split off the separator and element formatter */
			std::u32string_view format;
			bool escapedSeparator = false;
			for (size_t i = 0; i < fmt.size(); ++i) {
				if (fmt[i] != U'@')
					continue;
				if (++i >= fmt.size() || fmt[i] != U'@') {
					format = fmt.substr(i);
					fmt = fmt.substr(0, i - 1);
				}
				else
					escapedSeparator = true;
			}

			/* check if the separator contains formatting and sanitize it */
			if (!escapedSeparator)
				return { fmt, format };
			for (size_t i = 0; i < fmt.size(); ++i) {
				_buffer.push_back(fmt[i]);
				if (fmt[i] == U'@')
					++i;
			}
			return{ _buffer, format };
		}

		template <class ItType>
		bool FormatRangeOut(auto& sink, std::u32string_view format, std::u32string_view separator, const ItType& begin, const ItType& end) {
			bool separate = false;

			/* iterate over the elements and write them out */
			for (ItType it = begin; it != end; ++it) {
				if (separate && !separator.empty())
					str::FastcodeAllTo<str::CodeError::replace>(sink, separator);
				separate = true;

				/* format the value itself */
				if (!str::CallFormat(sink, *it, format))
					return false;
			}
			return true;
		}

		bool FormatRepeatOut(auto& sink, size_t count, std::u32string_view format, std::u32string_view separator, const auto& value) {
			/* check if the value is being written out at all or just once */
			if (count == 0)
				return true;
			if (count == 1)
				return str::CallFormat(sink, value, format);

			/* format the value to a temporary buffer */
			std::u32string fmtValue;
			if (!str::CallFormat(fmtValue, value, format))
				return false;

			/* iterate over the count and write the values and separators out */
			for (size_t i = 0; i < count; ++i) {
				if (i > 0 && !separator.empty())
					str::FastcodeAllTo<str::CodeError::replace>(sink, separator);
				str::FastcodeAllTo<str::CodeError::replace>(sink, fmtValue);
			}
			return true;
		}
	}

	/*	Normal padding but:
	*	=> inbetween alignment and minimum-digits:
	*		[-+ ]: character for sign-place
	*		[#]: add prefix
	*		[0]: Optional (if no alignment specified, to indicate null-padding)
	*	[bBqQoOdDxXsSuU]: radix and casing */
	template <str::IsInteger Type> struct Formatter<Type> {
		bool operator()(str::IsSink auto& sink, Type val, std::u32string_view fmt) const {
			fmt::Padding padding{};
			size_t consumed = 0;

			/* parse the initial padding alignment */
			consumed += fmt::ParsePaddingAlignment(fmt, padding);

			/* parse the number-preamble requirements (only allow null-padding if no alignment has been specified) */
			auto [_consumed0, signChar, prefix, nullPadding] = detail::ParseNumPreamble(fmt.substr(consumed), padding.align == fmt::Alignment::standard);
			consumed += _consumed0;

			/* parse the minimum and maximum padding state */
			consumed += fmt::ParsePaddingMinimum(fmt.substr(consumed), padding);
			consumed += fmt::ParsePaddingMaximum(fmt.substr(consumed), padding);

			/* parse the num-radix */
			auto [radix, upperCase, _radixFound] = detail::ParseNumRadix(fmt.substr(consumed), false, std::is_signed_v<Type>, !std::is_signed_v<Type>);
			if (_radixFound)
				++consumed;

			/* check if the entire format has been consumed */
			if (consumed < fmt.size())
				return false;
			str::ArgsInt intArgs = { .radix = radix, .style = (upperCase ? str::NumStyle::upper : str::NumStyle::lower) };

			/* check if the number can just be written out */
			if (padding.minimum <= 1 && padding.maximum == 0) {
				detail::NumPreambleInto<Type>(sink, val, signChar, radix, upperCase, prefix);
				str::IntTo(sink, val, intArgs);
				return true;
			}

			/* write the prefix to be used to the buffer */
			std::u32string buffer;
			detail::NumPreambleInto<Type>(buffer, val, signChar, radix, upperCase, prefix);

			/* check if the number is to be null-padded */
			if (nullPadding) {
				/* write the number to an intermediate buffer to estimate its size */
				std::u32string temp;
				str::IntTo(temp, val, intArgs);

				/* write the nulls to the buffer and write the number itself to the buffer */
				if (buffer.size() + temp.size() < padding.minimum)
					str::CodepointTo<str::CodeError::replace>(buffer, U'0', padding.minimum - buffer.size() - temp.size());
				str::FastcodeAllTo<str::CodeError::replace>(buffer, temp);
			}

			/* write the number to an intermediate buffer */
			else
				str::IntTo(buffer, val, intArgs);

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
	*	[egf]: style
	*		=> e: FloatStyle::scientific
	*		=> g: FloatStyle::general
	*		=> f: FloatStyle::fixed */
	template <str::IsFloat Type> struct Formatter<Type> {
		bool operator()(str::IsSink auto& sink, Type val, std::u32string_view fmt) const {
			/* parse the float formatting */
			detail::FltFormat flt = detail::ParseFltFormatting(fmt);

			/* check if the entire format has been consumed */
			if (flt.consumed < fmt.size())
				return false;
			str::ArgsFloat floatArgs = {
				.precision = flt.precision,
				.radix = flt.radix,
				.fltStyle = flt.style,
				.numStyle = (flt.upperCase ? str::NumStyle::upper : str::NumStyle::lower)
			};

			/* check if the number can just be written out */
			if (flt.padding.minimum <= 1 && flt.padding.maximum == 0) {
				detail::NumPreambleInto<Type>(sink, val, flt.signChar, flt.radix, flt.upperCase, flt.prefix);
				str::FloatTo(sink, val, floatArgs);
				return true;
			}

			/* write the float to the buffer and format it padded out */
			std::u32string buffer = detail::WriteFltBuffered(flt, val, floatArgs);
			fmt::WritePadded(sink, buffer, flt.padding);
			return true;
		}
	};

	/*	Normal padding
	*	[e]: escape: escape all non-printable characters using \x or \u{...} or common escape-sequences
	*	[a]: ascii: escape any non-ascii characters or control-characters using \x or \u{...} or common escape-sequences (if not in escape-mode) */
	template <str::IsStr Type> struct Formatter<Type> {
		bool operator()(str::IsSink auto& sink, const Type& t, std::u32string_view fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the string-formatting */
			auto [ascii, escape] = detail::ParseStrFormatting(rest);
			if (escape)
				rest = rest.substr(1);

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* check if the string can just be appended */
			if (!escape && padding.minimum <= 1 && padding.maximum == 0) {
				str::FastcodeAllTo<str::CodeError::replace>(sink, t);
				return true;
			}

			/* write the string to an intermediate buffer */
			std::u32string buffer;
			if (escape) {
				using ChType = str::StringChar<Type>;
				std::basic_string_view<ChType> view{ t };

				/* extract all separate characters */
				while (!view.empty()) {
					auto [cp, consumed] = str::GetCodepoint<str::CodeError::replace>(view);
					view = view.substr(consumed);

					/* create the escape sequence or write the character out as is */
					if (cp == str::Invalid)
						continue;
					if (ascii || !cp::prop::IsPrint(cp))
						str::EscapeTo(buffer, cp, true);
					else
						buffer.push_back(cp);
				}
			}
			else
				str::FastcodeAllTo<str::CodeError::replace>(buffer, t);

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding but:
	*	[#]: add prefix
	*	[u]: uppercase */
	template <> struct Formatter<const void*> {
		constexpr bool operator()(str::IsSink auto& sink, const void* val, std::u32string_view fmt) const {
			/* parse the remainder of the padding rules */
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* check if a prefix should be added */
			bool prefix = (!rest.empty() && rest[0] == U'#');
			if (prefix)
				rest = rest.substr(1);

			/* parse the final arguments and validate them */
			bool upperCase = false;
			if (!rest.empty() && rest[0] == U'u') {
				upperCase = true;
				rest = rest.substr(1);
			}

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* construct the output-string */
			str::Local<char32_t, sizeof(void*) * 2 + 2> buffer;
			if (prefix)
				buffer.append(str::MakePrefix(16, upperCase));
			for (size_t i = sizeof(void*) * 2; i > 0 && (uintptr_t(val) >> (i - 1) * 4) == 0; --i)
				buffer.push_back(U'0');
			str::IntTo(buffer, uintptr_t(val), { .radix = 16, .style = (upperCase ? str::NumStyle::upper : str::NumStyle::lower) });

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};
	template <> struct Formatter<void*> {
		constexpr bool operator()(str::IsSink auto& sink, void* val, std::u32string_view fmt) const {
			return str::Formatter<const void*>{}(sink, val, fmt);
		}
	};

	/*	Normal padding
	*	[sS]: as string (true/false vs True/False) */
	template <> struct Formatter<bool> {
		constexpr bool operator()(str::IsSink auto& sink, bool val, std::u32string_view fmt) const {
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
	*	[f]: format to use forward slashes
	*	[b]: format to use backward slashes */
	template <> struct Formatter<std::filesystem::path> {
		constexpr bool operator()(str::IsSink auto& sink, const std::filesystem::path& val, std::u32string_view fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the final arguments and validate them */
			char32_t rep = 0;
			if (!rest.empty()) {
				if (rest[0] == U'f') {
					rep = U'/';
					rest = rest.substr(1);
				}
				else if (rest[0] == U'b') {
					rep = U'\\';
					rest = rest.substr(1);
				}
			}

			/* check if the entire string has been processed */
			if (!rest.empty())
				return false;

			/* check if the string can just be appended */
			if (padding.minimum <= 1 && padding.maximum == 0 && rep == 0) {
				str::FastcodeAllTo<str::CodeError::replace>(sink, val.native());
				return true;
			}

			/* write the string to an intermediate buffer */
			std::u32string buffer;
			using ChType = str::StringChar<std::filesystem::path::string_type>;
			std::basic_string_view<ChType> view{ val.native() };

			/* extract all separate characters */
			while (!view.empty()) {
				auto [cp, consumed] = str::GetCodepoint<str::CodeError::replace>(view);
				view = view.substr(consumed);

				/* change the slash type accordingly */
				if (cp == str::Invalid)
					continue;
				if (rep == 0 || (cp != U'/' && cp != U'\\'))
					buffer.push_back(cp);
				else
					buffer.push_back(rep);
			}

			/* write the padded string to the sink */
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[@d+]: number of times to repeat char (default: 1)
	*	[e]: escape: escape all non-printable characters using \x or \u{...} or common escape-sequences
	*	[a]: ascii: escape any non-ascii characters or control-characters using \x or \u{...} or common escape-sequences (if not in escape-mode) */
	template <> struct Formatter<char> {
		constexpr bool operator()(str::IsSink auto& sink, char val, std::u32string_view fmt) const {
			return detail::FormatChar<char>(sink, val, fmt);
		}
	};
	template <> struct Formatter<wchar_t> {
		constexpr bool operator()(str::IsSink auto& sink, wchar_t val, std::u32string_view fmt) const {
			return detail::FormatChar<wchar_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char8_t> {
		constexpr bool operator()(str::IsSink auto& sink, char8_t val, std::u32string_view fmt) const {
			return detail::FormatChar<char8_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char16_t> {
		constexpr bool operator()(str::IsSink auto& sink, char16_t val, std::u32string_view fmt) const {
			return detail::FormatChar<char16_t>(sink, val, fmt);
		}
	};
	template <> struct Formatter<char32_t> {
		constexpr bool operator()(str::IsSink auto& sink, char32_t val, std::u32string_view fmt) const {
			return detail::FormatChar<char32_t>(sink, val, fmt);
		}
	};

	/*	Float formatting
	*	[_]: use space between value and unit
	*	[s]: use simple si-prefixes (i.e. ascii-only)
	*	[2]: use binary-system instead of normal decimal-system for the si-prefix
	*	[!]: append spaces for equal sizes, if no si-prefix is necessary */
	template <class NumType, class UnitType> struct Formatter<str::Si<NumType, UnitType>> {
		constexpr bool operator()(str::IsSink auto& sink, const str::Si<NumType, UnitType>& val, std::u32string_view fmt) const {
			/* parse the float formatting */
			detail::FltFormat flt = detail::ParseFltFormatting(fmt);
			fmt = fmt.substr(flt.consumed);

			/* check if a space should be inserted */
			bool space = fmt.starts_with(U'_');
			if (space)
				fmt = fmt.substr(1);

			/* check if the simple prefix should be used */
			bool simple = fmt.starts_with(U's');
			if (simple)
				fmt = fmt.substr(1);

			/* check if it should be the two-system */
			bool two = fmt.starts_with(U'2');
			if (two)
				fmt = fmt.substr(1);

			/* check if always a character should be inserted */
			bool always = fmt.starts_with(U'!');
			if (always)
				fmt = fmt.substr(1);

			/* check if the entire format has been consumed */
			if (!fmt.empty())
				return false;
			str::ArgsFloat floatArgs = {
				.precision = flt.precision,
				.radix = flt.radix,
				.fltStyle = flt.style,
				.numStyle = (flt.upperCase ? str::NumStyle::upper : str::NumStyle::lower)
			};

			/* compute the actual value to be formatted */
			str::SiScale scale = str::SiMakeScale(val.value, { .asciiOnly = simple, .binarySystem = two });
			double number = static_cast<double>(val.value) / scale.scale;

			/* check if the number can just be written out */
			if (flt.padding.minimum <= 1 && flt.padding.maximum == 0) {
				detail::NumPreambleInto<double>(sink, number, flt.signChar, flt.radix, flt.upperCase, flt.prefix);
				str::FloatTo(sink, number, floatArgs);
				detail::SiEpilogueInto(sink, scale.prefix, val.unit, space, always, two);
				return true;
			}

			/* write the value to the buffer and format it padded out */
			std::u32string buffer = detail::WriteFltBuffered(flt, number, floatArgs);
			detail::SiEpilogueInto(buffer, scale.prefix, val.unit, space, always, two);
			fmt::WritePadded(sink, buffer, flt.padding);
			return true;
		}
	};

	/* No formatting rules will be respected, as the internally stored rules will be applied to the argument */
	template <class FmtType, class Type> struct Formatter<str::As<FmtType, Type>> {
		constexpr bool operator()(str::IsSink auto& sink, const str::As<FmtType, Type>& val, std::u32string_view fmt) const {
			/* ensure that the formatting string is empty */
			if (!fmt.empty())
				return false;

			/* check if the formatter can be used directly */
			if constexpr (str::EffSame<str::StringChar<FmtType>, char32_t>)
				return str::CallFormat(sink, val.value, val.format);

			/* convert the formatting and write the value out */
			else {
				std::u32string buffer;
				str::FastcodeAllTo<str::CodeError::replace>(buffer, val.format);
				return str::CallFormat(sink, val.value, buffer);
			}
		}
	};

	/*	Normal padding
	*	[%any%]: separator to be used (escape @ using @@ within this part; default: '')
	*	[@%any%]: formatter to be used for the elements */
	template <class ItType> struct Formatter<str::Range<ItType>> {
		constexpr bool operator()(str::IsSink auto& sink, const str::Range<ItType>& val, std::u32string_view fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the separator and element formatter */
			std::u32string _separator;
			auto [separator, format] = detail::ParseElementFormatting(rest, _separator);
			bool separate = false;

			/* check if the string can just be appended */
			if (padding.minimum <= 0 && padding.maximum == 0)
				return detail::FormatRangeOut(sink, format, separator, val.begin, val.end);

			/* write the string to an intermediate buffer and write the padded string to the sink */
			std::u32string buffer;
			if (!detail::FormatRangeOut(buffer, format, separator, val.begin, val.end))
				return false;
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};

	/*	Normal padding
	*	[%any%]: separator to be used (escape @ using @@ within this part; default: '')
	*	[@%any%]: formatter to be used for the elements */
	template <class Type> struct Formatter<str::Repeat<Type>> {
		constexpr bool operator()(str::IsSink auto& sink, const str::Repeat<Type>& val, std::u32string_view fmt) const {
			auto [padding, rest] = fmt::ParsePadding(fmt);

			/* parse the separator and element formatter */
			std::u32string _separator;
			auto [separator, format] = detail::ParseElementFormatting(rest, _separator);

			/* check if the value can just be written out to the sink */
			if (padding.minimum <= 0 && padding.maximum == 0)
				return detail::FormatRepeatOut(sink, val.count, format, separator, val.value);

			/* write the string to an intermediate buffer and write the padded string to the sink */
			std::u32string buffer;
			if (!detail::FormatRepeatOut(buffer, val.count, format, separator, val.value))
				return false;
			fmt::WritePadded(sink, buffer, padding);
			return true;
		}
	};
}
