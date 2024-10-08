/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024 Bjoern Boss Henrichsen */
#pragma once

#include "str-local.h"
#include "str-bytes.h"
#include "str-chars.h"
#include "str-number.h"
#include "str-coding.h"
#include "str-string.h"
#include "str-wire.h"
#include "str-helper.h"
#include "str-escape.h"
#include "str-specializations.h"
#include "str-format.h"
#include "unicode/cp-casing.h"
#include "unicode/cp-normalization.h"
#include "unicode/cp-property.h"
#include "unicode/cp-segmentation.h"

namespace str {
	/* default string-type to be used [utf-16] */
	using UString = str::String<char16_t, err::DefChar>;

	/* default string-view-type to be used [utf-16] */
	using UView = str::View<char16_t, err::DefChar>;

	/* convenience for fast formatting to std::cout */
	constexpr void Fmt(const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatTo(std::cout, fmt, args...);
	}
	constexpr void FmtLn(const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatTo(std::cout, fmt, args...);
		std::cout << '\n';
	}

	/* convenience for fast formatting to std::wcout */
	constexpr void FmtW(const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatTo(std::wcout, fmt, args...);
	}
	constexpr void FmtWLn(const str::IsStr auto& fmt, const str::IsFormattable auto&... args) {
		str::FormatTo(std::wcout, fmt, args...);
		std::wcout << L'\n';
	}

	/* convenience for fast building to std::cout */
	constexpr void Print(const str::IsFormattable auto&... args) {
		str::BuildTo(std::cout, args...);
	}
	constexpr void PrintLn(const str::IsFormattable auto&... args) {
		str::BuildTo(std::cout, args...);
		std::cout << '\n';
	}

	/* convenience for fast building to std::wcout */
	constexpr void PrintW(const str::IsFormattable auto&... args) {
		str::BuildTo(std::wcout, args...);
	}
	constexpr void PrintWLn(const str::IsFormattable auto&... args) {
		str::BuildTo(std::wcout, args...);
		std::wcout << L'\n';
	}

	/* convenience to build an runtime exception */
	struct BuildException : public str::RuntimeException {
		template <class... Args>
		constexpr BuildException(const Args&... args) : str::RuntimeException{ str::Build<std::wstring>(args...) } {}
	};
}
