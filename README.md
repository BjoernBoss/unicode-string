# Unicode String Functions for C++
![C++](https://img.shields.io/badge/language-c%2B%2B20-blue?style=flat-square)
[![License](https://img.shields.io/badge/license-BSD--3--Clause-brightgreen?style=flat-square)](LICENSE.txt)

Header only library written in `C++20` to add support for various unicode operations and interoperability between all string types. Simply include [`ustring.h`](ustring.h) to add the entire functionality.

Core concepts: This library is not necessarily the fastest in every scenario, but it aspires to be versatile and usable for every case. This includes not making any assumptions about character encodings, supporting encoding and decoding from every character coding to every coding, while resulting in accurate decodings and encodings. This enables things such as:

```C++
std::string s = str::Build<std::string>("ab", u"cd", U"ef", L"gh", u8"ij", 50, 90.0f);
std::string t = str::View{ s }.upper();
std::u32string u = str::Float<std::u32string>(123.5f);
```
This results in `UTF` coding correct and `Unicode` based casing rules, without requiring additional libraries or such to be installed.

It includes functions to perform various unicode testing, segmentation, transformations, and analysis functions. Further, it adds the ability to parse and print numbers from any string type, format strings to any string type, serialize and deserialize string types to bytes, escape strings, and convert string types to each other as correctly as possible.

The library consideres any object to be convertible to a `string_view` of any type as a valid string.

## Using the library
This library is a header only library. Simply clone the repository, and include `<ustring/ustring.h>`

    $ git clone https://github.com/BjoernBoss/ustring.git

## Requirements for String Types
The library does have some requirements for the underlying types, which are also checked statically.

    char       Must have a size of 1 byte and either use utf-8 encoding or any multibyte encoding
    wchar_t    Must have a size of 2 or 4 bytes and either use utf-16 or utf-32 encoding
    char8_t    Must have a size of 1 byte and use utf-8 encoding
    char16_t   Must have a size of 2 bytes and use utf-16 encoding
    char32_t   Must have a size of 4 bytes and use utf-32 encoding

In case of `char` using utf-8 encoding, the decoding / encoding will be performed using the internal utf-8 coding functionalities. Otherwise `std::mbrtowc` and `str::wctomb` will be used. Similarly, for `wchar_t`, it will either use the internal utf-16 or utf-32 coding functions. For further optimizations, the library tries to determine at compile time, if the multibyte `char` maps all ascii-characters correctly. It exposes these properties in code:

    str::CharIsUtf8      Does the char encoding use utf-8, otherwise an unknown multibyte encoding
    str::CharHoldsAscii  Does the char encoding match ascii for the first 128 chars
    str::WideIsUtf16     Does the wchar_t encoding use utf-16
    str::WideIsUtf32     Does the wchar_t encoding use utf-32

Disclaimer: while this should work in theory, at least in `Visual Studio 2022` using the `execution-charset: IBM01149` still resulted in `a` being mapped to `0x61`, although it should have been mapped to `0x81`, which in turn resulted in the compile time ascii-detection to fail.

## Concepts and Interfaces
The library defines a set of concepts, which it frequently uses.

 - `str::IsChar` Check if the type is a supported character (`char`, `wchar_t`, ...)
 - `str::IsStr` Check if the type can be converted to a `string_view` (use `str::StringChar` to get the corresponding character-type)
 - `str::IsChStr<T>` Check if the type can be converted to a `basic_string_view<T>`
 - `str::IsData` Check if the type specialized the `str::ByteReader` (used for `str::FromWire` and such)
 - `str::IsSink` Check if the type specialized the `str::CharWriter` interface (used by all writing functions; use `str::SinkChar` to get the corresponding character-type)
 - `str::IsWire` Check if the type specialized the `str::ByteWriter` interface (used for `str::ToWire` and such)
 - `str::IsCharLoader` Check if the type specialized the `str::CharLoader` interface (used for character streaming such as `str::WireIn`)
 - `str::IsStream` Check if the type fulfills `str::IsStr` or `str::IsCharLoader` (used for `str::Stream` and such)
 - `str::IsByteLoader` Check if the type specialized the `str::ByteLoader` interface (used for byte streaming such as `str::WireIn`)
 - `str::IsSource` Check if the type fulfills `str::IsData` or `str::IsByteLoader` (used for `str::Source` and such)
 - `str::IsFormattable` Check if the type specialized the str::Formatter interface
 - `str::IsCPIterator` Check if the type is an iterator over codepoints
 - `str::IsReceiver<T...>` Check if the type can receive the types through its call operator
 - `str::IsCollector<T...>` Check if the type has a `next(T...)` and `done()` method to act as recipient for transformed codepoints
 - `str::IsMapper` Check if the type is a transformation and can instantiate a new `str::IsCollector<char32_t>` when receiving a `str::IsCollector<char32_t>` to write its output to
 - `str::IsAnalysis` Check if the type performs a boolean analysis on a stream of codepoints
 - `str::IsTester` Check if the type performs a boolean analysis on a single codepoint

## Sinks, Sources, and Streams

By default the library uses the concepts of sinks or wires to write to, and strings or data to read from. A sink is anything that fulfills `str::IsSink`, for which many default specializations exist. For convenience, further implementations, such as `str::NullChars`, `str::Chars`, and `str::InheritSink` exist, to allow using buffers and virtualized sinks. A sink defines its supported character type, and all operations, which write to the sink, will ensure only the proper type of characters is written out. Further, no partially encoded codepoints will be written out. They will always be written out in a single batch.

A wire is defined in the same way as `str::IsWire`, and also offers `str::Bytes` and `str::InheritWire` as support.

For both sinks and wires `str::BufferSink` and `str::BufferWire` exist, to write the data to an intermediate buffer, before flushing them out in one go. Otherwise, expensive write operations might result in performance penalties, as sinks and wires are both used in small increments by the library (such as on a per character or per byte level).

Similarily, `str::BufferStream` and `str::BufferSource` exist, to read more data in advance to an intermediate buffer, to reduce the number of operations to the lower source.

A source for strings is anything that is convertible to an `std::basic_string_view`. For bytes, it is any type that fulfills `str::IsData`, for which many default specializations exist. Similarly to sinks, strings also define their source character type, which also defines their encoding used.

To further support streaming of data, `str::IsStream` exists for character streams, and `str::IsSource` for byte streams. In order to use them properly, `str::Stream` or `str::Source` exist, which wrap the type, and build up internal cached states to prevent repeated fetching of data, and allowing lookaheads.

There exist various helper objects, which wrap the types, such as `str::BufferSink`. Generally speaking, the entire library is designed to either store a reference to the source object, if it is constructed with an `lvalue`, and store a move-constructed copy of the source object, if it is constructed with an `rvalue`. This allows the following two examples to compile, while retaining an expected lifetime:

```C++
str::BuildTo(str::BufferSink{ std::fstream("/some/file"), 40 }, "abc", '-', U"def");
std::string buffer;
str::BuildTo(str::BufferSink{ buffer, 60 }, "abc", '-', U"def");
```


## Standard Functionality
The standard string functionality lies in the namespace `str` for all string-related operations.

To allow local strings from being used, `str::Local<class CharType, ssize_t Capacity>` is provided, which allows to create a string on the stack of the given character type and the given capacity. 

For convenience, `str::ch` / `str::wd` / ... have been provided, which add common functionality but defaulted to either the corresponding char-type (i.e. `str::u8::Local<Capacity>` => `str::Local<char8_t, Capacity>`) or the corresponding string type (i.e. `str::u16::Build` => `str::Build<std::u16string>`). The same also has been done for `str::lch` / `str::lwd` / ..., which provide the common functionality, but for local strings (i.e. `str::lwd<32>::Int` => `str::Int<str::Local<Wd, 32>>`).

The idea of the library is to offer two functions of any kind, such as `str::Int` and `str::IntTo`, where the first function returns a new string-object of the result of the function, while the `To`-function writes the result to the character sink, which is passed in as the first argument.

### [str::String, str::View](str-string.h)
The primary functionality is combined into the `str::String` or `str::View` wrapper. They extend `std::basic_string` or `std::basic_string_view` accordingly, and extend it with the new functionality. They both provide the same interface, and will therefore only be referenced through `str::View` here. This functionality includes various testing functions, transformations, normalizations, and unicode normalized or case-folded comparisons. This also includes functions, such as `lstrip` or `rstrip`. As an example:

```C++
std::u16string s = str::View{ L"abc-def" }.norm().to<std::u16string>();
bool t = str::View{ U"\U0001F9D1\u200D\U0001F680" }.isEmoji();
```

### [str::Float, str::Int, str::ParseNum](format/str-number.h)
The number functions can parse any kind of number and produce float or integer-strings for any valid radix. The functions themselves can only be used with ascii-numbers. In order to use it for any other decimal-representation, such as arabic-indic digit `\u0664`, use the convenience function `str::View::asciiDecimals` function. Examples for interacting with numbers:

```C++
std::wstring s = str::Float<std::wstring>(50.0f, { .fltStyle = str::FloatStyle::general });
float f = str::ParseNum<float>(u8"-1523.23e+5").value;
```

### [str::Format, str::Build](format/str-format.h), [str::Fmt, str::Print](ustring.h)
For interacting with formatting, `str::Format` and `str::Build` exist, where `str::Build` is effectively defined as a format using the format-string `"{}{}{}..."`. Otherwise the default formatting rules mostly apply. Comments per type exist. Examples of using these:

```C++
std::u16string s = str::Format<std::u16string>(u8"Test: {:.^#10x}\n", 65536);
std::wstring t = str::Build<std::wstring>(1, true, "abc", u8'-', U"def");

/* using an argument to modify the argument-string dynamically (will effectively use: #020x on 512) */
std::string u = str::Format<std::string>(U"abc: {:#0{}{3}}\n", 512, 20, "unused", 'x');
```

For convenience, `str::Fmt`, `str::FmtLn`, `str::Print`, `str::PrintLn` exist to either format or build directly out to `std::cout`, or `std::wcout`, when using `str::FmtW`, ... To print ranges defined by iterators or `begin()` and `end()`, the `str::Range` helper can be used. To repeat a formatted value, `str::Repeat` can be used. Another addition is `str::Si` to format numerical values using si unit scaling, such as `k` for kilo, `M` for Mega and so on. Further, `str::As` exists, to allow a format to be specified and bound to an argument. This allows `str::Build` to be used, while simultaneously formatting a parameter:

```C++
std::u16string s = str::Build<std::u16string>(1, true, str::As{ "#010x", 50 }, u8"Def");
```

### [str::ToWire, str::FromWire](format/str-wire.h)
To encode or decode strings to raw bytes, the `str::ToWire` and `str::FromWire` exist. They both optionally add a `BOM` or try to determine the encoding type, based on an encountered `BOM`. Examples of using the functions:

```C++
/* reading from a byte-source */
uint8_t someBuffer[512] = /* ... */;
auto x = str::FromWire{ str::Coding::utf8, str::BOMMode::detectAll };
x.read<std::string>(someBuffer);

/* writing to a byte-source */
std::ofstream someFile = /* ... */;
auto y = str::ToWire{ str::Encoding::utf16leBOM };
y.write(someFile, u8"Some-String");
y.write(someFile, U" some other string");
```

Note: For convenience, `str::WireOut` exists as valid sink to feed the string output directly to a corresponding wire object. Similarly, `str::WireIn` exists to create a character-stream from a byte-source and immediately transcode the characters.

```C++
std::ofstream someFile = /* ... */;

str::FormatTo(str::WireOut{ someFile, str::Encoding::utf16leBOM }, "abc: {:#0{}x}\n", 12345, 19);
```

The wire operations can also be used directly through the `str::Encode` or `str::Decode` interfaces. This is also mapped into the `str::View`.

```C++
str::View{ "abc" }.encode(str::Encoding::utf8);
```

### [str::Stream](common/str-chars.h), [str::Source](common/str-bytes.h)
The `str::Stream` object creates a wrapper around a type, which implements the character-source `str::IsStream`. `str::Source` does the same, but for any type, which implements the byte-source interface `str::IsSource`. Example of using the stream:

```C++
str::Stream stream{ u8"Some-String" };
while (!stream.done()) {
    str::PrintLn(stream.load(64));
    stream.consume();
}
```

The `str::U32Stream` wrapper implements a stream, which allows to create a stream, which decodes any source stream to a stream of type char32_t. The `str::LimitStream` and `str::LimitSource` wrapper implement a source or stream, which limits the number of tokens to be consumed.

Similarly, `str::InheritSource` and `str::InheritStream` exist, alongside with `str::SourceImplementation` and `str::StreamImplementation`. These allow to create virtualized sources or streams, thus preventing to use templates everywhere.

### [str::Iterator, str::CPRange](coding/str-coding.h)
The `str::Iterator` provides a codepoint iterator, and `str::CPRange` a corresponding range for it, which allows iteration both forward and backward over the encoded codepoints. The iterator can immediately be instantiated through `str::View::codepoints` or `str::View::cpBegin` / `str::View::cpEnd`. Example of using the iterators:

```C++
for (char32_t cp : str::View{ u"abc-def" }.codepoints())
    foo(cp);
```
Note: To ensure the iterator starts at an codepoint aligned index, use `str::TestCodepoint` or `str::View::isAligned`.

## [Coding Error Handling](coding/str-coding.h)
Any encoding or decoding errors will be handled according to the `Error` template parameter. Most corresponding functions in the `str` namespace will have the `Error` parameter, which is defaulted to `str::CodeError::replace`. The following values are defined:

    str::CodeError::replace   Replace any invalid codepoints with the unicode replacement character or '?'
    str::CodeError::exception Throw an error if an encoding or decoding error is encountered
    str::CodeError::nothing   Return str::Invalid as codepoint if an error is encountered
    %any%                     Replace any invalid codepoints with the value %any%

## Unicode Functionality
The unicode related operations lie in the namespace `cp`. All codepoints are represented using `char32_t` as type.

### [cp::prop, cp::ascii](unicode/cp-property.h)
The `cp::prop` and `cp::ascii` namespaces contain various test functions to query properties per single codepoint. Most functions are directly integrated into `str::View`. The `IsUpper` or `IsLower` functions should not be used to determine if the string is uppercased or lowercased, as they only perform checks based on the unicode properties, without respecting any surrouunding context. Examples of using the properties:

```C++
cp::prop::GCType gc = cp::prop::GetCategory(U'a');
bool s = cp::prop::IsAssigned(U'\U0010ff00');
size_t t = cp::ascii::GetRadix(U'9');
```

### [cp::UpperCase, cp::LowerCase, cp::TitleCase, cp::FoldCase](unicode/cp-casing.h)
The objects for transforming the casing of the string all adhere to the `str::IsMapper` concept. They take an optional locale as constructor argument, and can then be used to instantiate a mapper object into any sink. For each of the transformations, a corresponding tester exists, such as `cp::TestUpperCase`, which checks if the given string would not be modified by the corresponding mapper anymore. All of these functions are directly introduced to `str::View`. Example usage:

```C++
/* mapping a string to uppercase */
auto cps = str::View{ u"abc-def" }.codepoints();
std::string out;
auto map = cp::UpperCase{ "en_us" }(str::Collect(out));
for (char32_t cp : cps)
    map.next(cp);
map.done();

/* testing a string to be uppercase */
auto tester = cp::TestUpperCase{};
for (char32_t cp : cps)
    tester.next(cp);
bool t = tester.done();

/* usage through a view */
auto s0 = str::View{ u"abc-def" }.upper();
auto t0 = str::View{ u"abc-def" }.isLower();
```

Note: For convenience, `str::Collect` and `str::ForEach` exist as valid collectors to feed the output of the mappers either directly to a sink-object or to a lambda function.

### [cp::Decompose, cp::Compose, cp::NormFold](unicode/cp-normalization.h)
Similar to the casing functionalities, the transformations to convert a stream of codepoints to the normalized composed `NFC` or decomposed versions `NFD` exist, as well as to convert the stream to a case-folded and decomposed stream.

### [Graphemes, Words, Sentences, Lines](unicode/cp-segmentation.h)
For each of the kinds of separations, there exist multiple implementations. For graphemes, for example: `cp::GraphemeBreak`, `cp::GraphemeRanges`, `cp::GraphemeBefore`, `cp::GraphemeAfter`, `cp::GraphemeCheck`.

The Break object allows to instantiate a collector, which produces the corresponding break-type between any two codepoints. The Ranges object will produce a list of ranges of break-type none, as well as the last break-type before starting the given range. Both of these work on the correspoinding stream of codepoints from front to back, but are optimized for this. They take the codepoint and corresponding index, and will reference all results using the index. This allows strings to be used, which might require more than one source-character to encode a codepoint. Example usage:

```C++
std::u32string str = U"abc def";
std::vector<cp::Range> ranges;

auto words = cp::WordRanges{}([&](const cp::Range& r) { ranges.push_back(r); });
for (size_t i = 0; i < str.size(); ++i)
    words.next(str[i], i);
words.done();

/* also implemented via str::View */
std::vector<cp::Range> ranges2 = str::View{ "abc def" }.words();
```

The three remaining variants (before, after, check) will allow to perform arbitrary checks starting at any point and return the break-type according to the name, but it is less efficient than the other two alternatives. It operates based on a codepoint iterator passed to it. Example usage:

```C++
auto cps = str::View{ "abc def" }.codepoints();
cp::BreakKind brk = cp::WordBefore(cps.begin(), cps.at(4), cps.end());
```

## Unicode Data and Properties
All unicode properties are fetched from `Unicode Character Base`. All required files will be downloaded to `generated/ucd`. To fetch the latest release, run:

    $ py generated/generate.py --case --property --segment --normal

This will download all relevant files, parse them, and generate all corresponding files to `generated/unicode-*`. Both the generation script and all functions using the generated functions will perform static checks of the current state, to ensure as best as possible that the used algorithms are still valid.

To run the tests provided by the `Unicode Character Base`, as well as the number parsing and creating, checkout the `test` branch. Afterwards, generate the relevant test files and compile and run the test application.

    $ git checkout test
    $ py generated/generate.py --tests
    $ g++ -std=c++20 ./ustring-tests.cpp -O3 -o ustring-tests
    $ ./ustring-tests

Should errors occur, please create an issue for the github repository at [`https://github.com/BjoernBoss/ustring`](https://github.com/BjoernBoss/ustring).
