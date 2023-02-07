// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "Row.hpp"

#include <icu.h>

#include "textBuffer.hpp"
#include "../../types/inc/GlyphWidth.hpp"

#define DEBUG 1
#define ASSERT(b)           \
    do                      \
    {                       \
        if (!(b))           \
            __debugbreak(); \
    } while (false)

#pragma comment(lib, "icu.lib")

using UniqueUBreakIterator = wil::unique_any<UBreakIterator*, decltype(&ubrk_close), &ubrk_close>;
static const UniqueUBreakIterator icuBreakIterator = []() noexcept {
    UErrorCode error = U_ZERO_ERROR;
    UniqueUBreakIterator iterator{ ubrk_open(UBRK_CHARACTER, "", nullptr, 0, &error) };
    FAIL_FAST_IF_MSG(error > U_ZERO_ERROR, "ubrk_open failed with %hs", u_errorName(error));
    return iterator;
}();

// The STL is missing a std::iota_n analogue for std::iota, so I made my own.
template<typename OutIt, typename Diff, typename T>
constexpr OutIt iota_n(OutIt dest, Diff count, T val)
{
    for (; count; --count, ++dest, ++val)
    {
        *dest = val;
    }
    return dest;
}

// ROW::ReplaceCharacters needs to calculate `val + count` after
// calling iota_n() and this function achieves both things at once.
template<typename OutIt, typename Diff, typename T>
constexpr OutIt iota_n_mut(OutIt dest, Diff count, T& val)
{
    for (; count; --count, ++dest, ++val)
    {
        *dest = val;
    }
    return dest;
}

// Same as std::fill, but purpose-built for very small `last - first`
// where a trivial loop outperforms vectorization.
template<typename FwdIt, typename T>
constexpr FwdIt fill_small(FwdIt first, FwdIt last, const T val)
{
    for (; first != last; ++first)
    {
        *first = val;
    }
    return first;
}

// Same as std::fill_n, but purpose-built for very small `count`
// where a trivial loop outperforms vectorization.
template<typename OutIt, typename Diff, typename T>
constexpr OutIt fill_n_small(OutIt dest, Diff count, const T val)
{
    for (; count; --count, ++dest)
    {
        *dest = val;
    }
    return dest;
}

// Same as std::copy_n, but purpose-built for very short `count`
// where a trivial loop outperforms vectorization.
template<typename InIt, typename Diff, typename OutIt>
constexpr OutIt copy_n_small(InIt first, Diff count, OutIt dest)
{
    for (; count; --count, ++dest, ++first)
    {
        *dest = *first;
    }
    return dest;
}

RowTextIterator::RowTextIterator(std::span<const wchar_t> chars, std::span<const uint16_t> charOffsets, uint16_t offset) noexcept :
    _chars{ chars },
    _charOffsets{ charOffsets },
    _beg{ offset },
    _end{ offset }
{
    operator++();
}

bool RowTextIterator::operator==(const RowTextIterator& other) const noexcept
{
    return _beg == other._beg;
}

RowTextIterator& RowTextIterator::operator++() noexcept
{
    _beg = _end;
    // Safety: _end won't be incremented past _columnCount, because the last
    // _charOffset at index _columnCount will never get the CharOffsetsTrailer flag.
    while (_uncheckedIsTrailer(++_end))
    {
    }
    return *this;
}

const RowTextIterator& RowTextIterator::operator*() const noexcept
{
    return *this;
}

std::wstring_view RowTextIterator::Text() const noexcept
{
    const auto beg = _uncheckedCharOffset(_beg);
    const auto end = _uncheckedCharOffset(_end);
    return { _chars.data() + beg, gsl::narrow_cast<size_t>(end - beg) };
}

til::CoordType RowTextIterator::Cols() const noexcept
{
    return _end - _beg;
}

DbcsAttribute RowTextIterator::DbcsAttr() const noexcept
{
    return Cols() == 2 ? DbcsAttribute::Leading : DbcsAttribute::Single;
}

// Safety: col must be [0, _columnCount].
uint16_t RowTextIterator::_uncheckedCharOffset(size_t col) const noexcept
{
    assert(col <= _charOffsets.size());
    return til::at(_charOffsets, col) & CharOffsetsMask;
}

// Safety: col must be [0, _columnCount].
bool RowTextIterator::_uncheckedIsTrailer(size_t col) const noexcept
{
    assert(col <= _charOffsets.size());
    return WI_IsFlagSet(til::at(_charOffsets, col), CharOffsetsTrailer);
}

// Routine Description:
// - constructor
// Arguments:
// - rowWidth - the width of the row, cell elements
// - fillAttribute - the default text attribute
// Return Value:
// - constructed object
ROW::ROW(wchar_t* charsBuffer, uint16_t* charOffsetsBuffer, uint16_t rowWidth, const TextAttribute& fillAttribute) :
    _charsBuffer{ charsBuffer },
    _chars{ charsBuffer, rowWidth },
    _charOffsets{ charOffsetsBuffer, ::base::strict_cast<size_t>(rowWidth) + 1u },
    _attr{ rowWidth, fillAttribute },
    _columnCount{ rowWidth }
{
    if (_chars.data())
    {
        _init();
    }
}

void swap(ROW& lhs, ROW& rhs) noexcept
{
    std::swap(lhs._charsBuffer, rhs._charsBuffer);
    std::swap(lhs._charsHeap, rhs._charsHeap);
    std::swap(lhs._chars, rhs._chars);
    std::swap(lhs._charOffsets, rhs._charOffsets);
    std::swap(lhs._attr, rhs._attr);
    std::swap(lhs._columnCount, rhs._columnCount);
    std::swap(lhs._lineRendition, rhs._lineRendition);
    std::swap(lhs._wrapForced, rhs._wrapForced);
    std::swap(lhs._doubleBytePadded, rhs._doubleBytePadded);
}

void ROW::SetWrapForced(const bool wrap) noexcept
{
    _wrapForced = wrap;
}

bool ROW::WasWrapForced() const noexcept
{
    return _wrapForced;
}

void ROW::SetDoubleBytePadded(const bool doubleBytePadded) noexcept
{
    _doubleBytePadded = doubleBytePadded;
}

bool ROW::WasDoubleBytePadded() const noexcept
{
    return _doubleBytePadded;
}

void ROW::SetLineRendition(const LineRendition lineRendition) noexcept
{
    _lineRendition = lineRendition;
}

LineRendition ROW::GetLineRendition() const noexcept
{
    return _lineRendition;
}

RowTextIterator ROW::Begin() const noexcept
{
    return { _chars, _charOffsets, 0 };
}

RowTextIterator ROW::End() const noexcept
{
    return { _chars, _charOffsets, _columnCount };
}

// Routine Description:
// - Sets all properties of the ROW to default values
// Arguments:
// - Attr - The default attribute (color) to fill
// Return Value:
// - <none>
void ROW::Reset(const TextAttribute& attr)
{
    _charsHeap.reset();
    _chars = { _charsBuffer, _columnCount };
    _attr = { _columnCount, attr };
    _lineRendition = LineRendition::SingleWidth;
    _wrapForced = false;
    _doubleBytePadded = false;
    _init();
}

void ROW::_init() noexcept
{
    std::fill_n(_chars.begin(), _columnCount, UNICODE_SPACE);
    std::iota(_charOffsets.begin(), _charOffsets.end(), uint16_t{ 0 });
}

// Routine Description:
// - resizes ROW to new width
// Arguments:
// - charsBuffer - a new backing buffer to use for _charsBuffer
// - charOffsetsBuffer - a new backing buffer to use for _charOffsets
// - rowWidth - the new width, in cells
// - fillAttribute - the attribute to use for any newly added, trailing cells
void ROW::Resize(wchar_t* charsBuffer, uint16_t* charOffsetsBuffer, uint16_t rowWidth, const TextAttribute& fillAttribute)
{
    // A default-constructed ROW has no cols/chars to copy.
    // It can be detected by the lack of a _charsBuffer (among others).
    //
    // Otherwise, this block figures out how much we can copy into the new `rowWidth`.
    uint16_t colsToCopy = 0;
    uint16_t charsToCopy = 0;
    if (_charsBuffer)
    {
        colsToCopy = std::min(rowWidth, _columnCount);
        // Safety: colsToCopy is [0, _columnCount].
        charsToCopy = _uncheckedCharOffset(colsToCopy);
        // Safety: colsToCopy is [0, _columnCount] due to colsToCopy != 0.
        for (; colsToCopy != 0 && _uncheckedIsTrailer(colsToCopy); --colsToCopy)
        {
        }
    }

    // If we grow the row width, we have to append a bunch of whitespace.
    // `trailingWhitespace` stores that amount.
    // Safety: The preceding block left colsToCopy in the range [0, rowWidth].
    const uint16_t trailingWhitespace = rowWidth - colsToCopy;

    // Allocate memory for the new `_chars` array.
    // Use the provided charsBuffer if possible, otherwise allocate a `_charsHeap`.
    std::unique_ptr<wchar_t[]> charsHeap;
    std::span chars{ charsBuffer, rowWidth };
    const std::span charOffsets{ charOffsetsBuffer, ::base::strict_cast<size_t>(rowWidth) + 1u };
    if (const uint16_t charsCapacity = charsToCopy + trailingWhitespace; charsCapacity > rowWidth)
    {
        charsHeap = std::make_unique_for_overwrite<wchar_t[]>(charsCapacity);
        chars = { charsHeap.get(), charsCapacity };
    }

    // Copy chars and charOffsets over.
    {
        const auto it = std::copy_n(_chars.begin(), charsToCopy, chars.begin());
        std::fill_n(it, trailingWhitespace, L' ');
    }
    {
        const auto it = std::copy_n(_charOffsets.begin(), colsToCopy, charOffsets.begin());
        // The _charOffsets array is 1 wider than newWidth indicates.
        // This is because the extra column contains the past-the-end index into _chars.
        iota_n(it, trailingWhitespace + 1u, charsToCopy);
    }

    _charsBuffer = charsBuffer;
    _charsHeap = std::move(charsHeap);
    _chars = chars;
    _charOffsets = charOffsets;
    _columnCount = rowWidth;

    // .resize_trailing_extent() doesn't work if the vector is empty,
    // since there's no trailing item that could be extended.
    if (_attr.empty())
    {
        _attr = { rowWidth, fillAttribute };
    }
    else
    {
        _attr.resize_trailing_extent(rowWidth);
    }
}

void ROW::TransferAttributes(const til::small_rle<TextAttribute, uint16_t, 1>& attr, til::CoordType newWidth)
{
    _attr = attr;
    _attr.resize_trailing_extent(gsl::narrow<uint16_t>(newWidth));
}

// Routine Description:
// - clears char data in column in row
// Arguments:
// - column - 0-indexed column index
// Return Value:
// - <none>
void ROW::ClearCell(const til::CoordType column)
{
    static constexpr std::wstring_view space{ L" " };
    ReplaceCharacters(column, 1, space);
}

// Routine Description:
// - writes cell data to the row
// Arguments:
// - it - custom console iterator to use for seeking input data. bool() false when it becomes invalid while seeking.
// - index - column in row to start writing at
// - wrap - change the wrap flag if we hit the end of the row while writing and there's still more data in the iterator.
// - limitRight - right inclusive column ID for the last write in this row. (optional, will just write to the end of row if nullopt)
// Return Value:
// - iterator to first cell that was not written to this row.
OutputCellIterator ROW::WriteCells(OutputCellIterator it, const til::CoordType columnBegin, const std::optional<bool> wrap, std::optional<til::CoordType> limitRight)
{
    THROW_HR_IF(E_INVALIDARG, columnBegin >= size());
    THROW_HR_IF(E_INVALIDARG, limitRight.value_or(0) >= size());

    // If we're given a right-side column limit, use it. Otherwise, the write limit is the final column index available in the char row.
    const auto finalColumnInRow = limitRight.value_or(size() - 1);

    auto currentColor = it->TextAttr();
    uint16_t colorUses = 0;
    auto colorStarts = gsl::narrow_cast<uint16_t>(columnBegin);
    auto currentIndex = colorStarts;

    while (it && currentIndex <= finalColumnInRow)
    {
        // Fill the color if the behavior isn't set to keeping the current color.
        if (it->TextAttrBehavior() != TextAttributeBehavior::Current)
        {
            // If the color of this cell is the same as the run we're currently on,
            // just increment the counter.
            if (currentColor == it->TextAttr())
            {
                ++colorUses;
            }
            else
            {
                // Otherwise, commit this color into the run and save off the new one.
                // Now commit the new color runs into the attr row.
                _attr.replace(colorStarts, currentIndex, currentColor);
                currentColor = it->TextAttr();
                colorUses = 1;
                colorStarts = currentIndex;
            }
        }

        // Fill the text if the behavior isn't set to saying there's only a color stored in this iterator.
        if (it->TextAttrBehavior() != TextAttributeBehavior::StoredOnly)
        {
            const auto fillingFirstColumn = currentIndex == 0;
            const auto fillingLastColumn = currentIndex == finalColumnInRow;
            const auto attr = it->DbcsAttr();
            const auto& chars = it->Chars();

            switch (attr)
            {
            case DbcsAttribute::Leading:
                if (fillingLastColumn)
                {
                    // The wide char doesn't fit. Pad with whitespace.
                    // Don't increment the iterator. Instead we'll return from this function and the
                    // caller can call WriteCells() again on the next row with the same iterator position.
                    ClearCell(currentIndex);
                    SetDoubleBytePadded(true);
                }
                else
                {
                    ReplaceCharacters(currentIndex, 2, chars);
                    ++it;
                }
                break;
            case DbcsAttribute::Trailing:
                // Handling the trailing half of wide chars ensures that we correctly restore
                // wide characters when a user backs up and restores the viewport via CHAR_INFOs.
                if (fillingFirstColumn)
                {
                    // The wide char doesn't fit. Pad with whitespace.
                    // Ignore the character. There's no correct alternative way to handle this situation.
                    ClearCell(currentIndex);
                }
                else
                {
                    ReplaceCharacters(currentIndex - 1, 2, chars);
                }
                ++it;
                break;
            default:
                ReplaceCharacters(currentIndex, 1, chars);
                ++it;
                break;
            }

            // If we're asked to (un)set the wrap status and we just filled the last column with some text...
            // NOTE:
            //  - wrap = std::nullopt    --> don't change the wrap value
            //  - wrap = true            --> we're filling cells as a steam, consider this a wrap
            //  - wrap = false           --> we're filling cells as a block, unwrap
            if (wrap.has_value() && fillingLastColumn)
            {
                // set wrap status on the row to parameter's value.
                SetWrapForced(*wrap);
            }
        }
        else
        {
            ++it;
        }

        // Move to the next cell for the next time through the loop.
        ++currentIndex;
    }

    // Now commit the final color into the attr row
    if (colorUses)
    {
        _attr.replace(colorStarts, currentIndex, currentColor);
    }

    return it;
}

bool ROW::SetAttrToEnd(const til::CoordType columnBegin, const TextAttribute attr)
{
    _attr.replace(_clampedColumnInclusive(columnBegin), _attr.size(), attr);
    return true;
}

void ROW::ReplaceAttributes(const til::CoordType beginIndex, const til::CoordType endIndex, const TextAttribute& newAttr)
{
    _attr.replace(_clampedColumnInclusive(beginIndex), _clampedColumnInclusive(endIndex), newAttr);
}

til::CoordType ROW::PrecedingColumn(til::CoordType column) const noexcept
{
    auto col = _clampedColumn(column);
    while (col != 0 && _uncheckedIsTrailer(--col))
    {
    }
    return col;
}

void ROW::ReplaceCharacters(til::CoordType columnBegin, std::wstring_view chars, til::CoordType width)
{
    uint16_t buffer[]{ 0, _clampedUint16(width) };
    std::span<uint16_t> charOffset{ buffer };
    ReplaceCharacters(columnBegin, til::CoordTypeMax, chars, &charOffset);
}

til::CoordType ROW::ReplaceCharacters(til::CoordType columnBegin, std::wstring_view& chars)
{
    return ReplaceCharacters(columnBegin, til::CoordTypeMax, chars, nullptr);
}

til::CoordType ROW::ReplaceCharacters(til::CoordType columnBegin, til::CoordType columnEnd, std::wstring_view& chars, std::span<uint16_t>* charOffsets)
try
{
#if 0
    const auto colBeg = _clampedUint16(columnBegin);
    const auto colEnd = _clampedUint16(columnBegin + width);

    if (colBeg >= colEnd || colEnd > _columnCount || chars.empty())
    {
        return;
    }

    // Extend range downwards (leading whitespace)
    uint16_t colExtBeg = colBeg;
    // Safety: colExtBeg is [0, _columnCount], because colBeg is.
    const uint16_t chExtBeg = _uncheckedCharOffset(colExtBeg);
    // Safety: colExtBeg remains [0, _columnCount] due to colExtBeg != 0.
    for (; colExtBeg != 0 && _uncheckedIsTrailer(colExtBeg); --colExtBeg)
    {
    }

    // Extend range upwards (trailing whitespace)
    uint16_t colExtEnd = colEnd;
    // Safety: colExtEnd cannot be incremented past _columnCount, because the last
    // _charOffset at index _columnCount will never get the CharOffsetsTrailer flag.
    for (; _uncheckedIsTrailer(colExtEnd); ++colExtEnd)
    {
    }
    // Safety: After the previous loop colExtEnd is [0, _columnCount].
    const uint16_t chExtEnd = _uncheckedCharOffset(colExtEnd);

    const uint16_t leadingSpaces = colBeg - colExtBeg;
    const uint16_t trailingSpaces = colExtEnd - colEnd;
    const size_t chExtEndNew = chars.size() + leadingSpaces + trailingSpaces + chExtBeg;

    if (chExtEndNew != chExtEnd)
    {
        _resizeChars(colExtEnd, chExtBeg, chExtEnd, chExtEndNew);
    }

    // Add leading/trailing whitespace and copy chars
    {
        auto it = _chars.begin() + chExtBeg;
        it = fill_n_small(it, leadingSpaces, L' ');
        it = copy_n_small(chars.begin(), chars.size(), it);
        it = fill_n_small(it, trailingSpaces, L' ');
    }
    // Update char offsets with leading/trailing whitespace and the chars columns.
    {
        auto chPos = chExtBeg;
        auto it = _charOffsets.begin() + colExtBeg;

        it = iota_n_mut(it, leadingSpaces, chPos);

        *it++ = chPos;
        it = fill_small(it, _charOffsets.begin() + colEnd, gsl::narrow_cast<uint16_t>(chPos | CharOffsetsTrailer));
        chPos = gsl::narrow_cast<uint16_t>(chPos + chars.size());

        it = iota_n_mut(it, trailingSpaces, chPos);
    }
#endif

    const auto colBeg = _clampedColumnInclusive(columnBegin);
    const auto colEnd = _clampedColumnInclusive(columnEnd);

    if (colBeg >= colEnd || chars.empty())
    {
        return colBeg;
    }
    
    // Safety:
    // * colBeg is now [0, _columnCount)
    // * colEnd is now (colBeg, _columnCount]

    // Algorithm explanation
    //
    // Task:
    //   Replace the characters in cells [colBeg, colEnd) with a single `width`-wide glyph consisting of `chars`.
    //
    // Problem:
    //   Imagine that we have the following ROW contents:
    //     "xxyyzz"
    //   xx, yy, zz are 2 cell wide glyphs. We want to insert a 2 cell wide glyph ww at colBeg 1:
    //       ^^
    //       ww
    //   An incorrect result would be:
    //     "xwwyzz"
    //   The half cut off x and y glyph wouldn't make much sense, so we need to fill them with whitespace:
    //     " ww zz"
    //
    // Solution:
    //   Given the range we want to replace [colBeg, colEnd), we "extend" it to encompass leading (preceding)
    //   and trailing wide glyphs we partially overwrite resulting in the range [colExtBeg, colExtEnd), where
    //   colExtBeg <= colBeg and colExtEnd >= colEnd. In other words, the to be replaced range has been "extended".
    //   The amount of leading whitespace we need to insert is thus colBeg - colExtBeg
    //   and the amount of trailing whitespace colExtEnd - colEnd.

#if DEBUG
    ASSERT(_chars.size() < 256);
    ASSERT(_columnCount < 256);
    wchar_t charsBackup[256];
    uint16_t charOffsetsBackup[256];
    std::copy_n(_chars, _chars.size(), &charsBackup[0]);
    std::copy_n(_charOffsets, _columnCount, &charOffsetsBackup[0]);
#endif

    // Extend range downwards (leading whitespace)
    uint16_t colExtBeg = colBeg;
    // Safety: colExtBeg is [0, _columnCount], because colBeg is.
    const uint16_t chExtBeg = _uncheckedCharOffset(colExtBeg);
    // Safety: colExtBeg remains [0, _columnCount] due to colExtBeg != 0.
    for (; colExtBeg != 0 && _uncheckedIsTrailer(colExtBeg); --colExtBeg)
    {
    }
    const uint16_t leadingSpaces = colBeg - colExtBeg;

    const uint16_t ch1 = chExtBeg + leadingSpaces;
    uint16_t ch2 = ch1;
    uint16_t col2 = colBeg;
    uint16_t paddingSpaces = 0;

    const auto beg = chars.begin();
    const auto end = chars.end();
    auto it = beg;
    {
        // ASCII "fast" pass
        const auto asciiMax = beg + std::min<size_t>(_columnCount - col2, chars.size());
        const auto asciiEnd = std::find_if(beg, asciiMax, [](const auto& ch) { return ch >= 0x80; });
        for (; it != asciiEnd; ++it, ++col2, ++ch2)
        {
            _charOffsets[col2] = ch2;
        }

        // Regular Unicode processing
        if (it != asciiMax)
        {
            // TODO backoff explain
            if (it != beg)
            {
                --it;
                --col2;
                --ch2;
            }

            const auto text16 = reinterpret_cast<const char16_t*>(&*it);
            const auto text16Length = gsl::narrow<int32_t>(end - it);

            UErrorCode error = U_ZERO_ERROR;
            ubrk_setText(icuBreakIterator.get(), text16, text16Length, &error);
            THROW_HR_IF_MSG(E_UNEXPECTED, error > U_ZERO_ERROR, "ubrk_setText failed with %hs", u_errorName(error));

            for (int32_t ubrk0 = 0, ubrk1; (ubrk1 = ubrk_next(icuBreakIterator.get())) != UBRK_DONE; ubrk0 = ubrk1)
            {
                const auto advance = _clampedUint16(ubrk1 - ubrk0);
                //(UEastAsianWidth)u_getIntPropertyValue(input, UCHAR_EAST_ASIAN_WIDTH);
                auto width = 1 + IsGlyphFullWidth({ &*it, advance });

                if (width > _columnCount - col2)
                {
                    SetDoubleBytePadded(true);
                    // Normally this should be something like `col2 + width - _columnCount`.
                    // But `width` can only ever be either 1 or 2 which means paddingSpaces can only be 1.
                    paddingSpaces = 1;
                    break;
                }

                _charOffsets[col2] = ch2;
                col2++;

                if (width != 1)
                {
                    _charOffsets[col2] = gsl::narrow_cast<uint16_t>(ch2 | CharOffsetsTrailer);
                    col2++;
                }

                ASSERT(static_cast<uint16_t>(ch2 + advance) > ch2);

                it += advance;
                ch2 += advance;

                if (col2 == _columnCount)
                {
                    break;
                }
            }
        }
    }

    uint16_t col3 = col2 + paddingSpaces;
    for (; _uncheckedIsTrailer(col3); ++col3)
    {
    }
    const uint16_t ch3 = _uncheckedCharOffset(col3);
    const uint16_t trailingSpaces = col3 - col2;

    const size_t copiedChars = ch2 - ch1;
    const size_t insertedChars = copiedChars + leadingSpaces + trailingSpaces;
    const size_t ch3new = insertedChars + chExtBeg;

    if (ch3new != ch3)
    {
        _resizeChars(col3, chExtBeg, ch3, ch3new);
    }

    {
        std::fill_n(_chars.begin() + chExtBeg, leadingSpaces, L' ');
        std::iota(_charOffsets.begin() + colExtBeg, _charOffsets.begin() + colBeg, chExtBeg);

        std::copy_n(chars.data(), copiedChars, _chars.begin() + ch1);

        std::fill_n(_chars.begin() + ch2, trailingSpaces, L' ');
        std::iota(_charOffsets.begin() + col2, _charOffsets.begin() + col3 + 1, ch2);
    }

    ASSERT(_uncheckedCharOffset(0) == 0);

    for (uint16_t i = 0; i <= _columnCount; ++i)
    {
        ASSERT(_uncheckedCharOffset(i) <= _chars.size());
        if (i)
        {
            const auto delta = _uncheckedCharOffset(i) - _uncheckedCharOffset(i - 1);
            ASSERT(delta >= 0 && delta <= 20);
            if (!delta)
            {
                ASSERT(_uncheckedIsTrailer(i));
            }
        }
    }

#if DEBUG
    {
        auto it = Begin();
        const auto end = End();

        UErrorCode error = U_ZERO_ERROR;
        ubrk_setText(icuBreakIterator.get(), reinterpret_cast<const char16_t*>(_chars.data()), _chars.size(), &error);
        THROW_HR_IF_MSG(E_UNEXPECTED, error > U_ZERO_ERROR, "ubrk_setText failed with %hs", u_errorName(error));

        for (int32_t ubrk0 = 0, ubrk1; (ubrk1 = ubrk_next(icuBreakIterator.get())) != UBRK_DONE; ubrk0 = ubrk1)
        {
            ASSERT(it != end);

            const auto expectedCols = it.Cols();
            const auto expectedText = it.Text();
            const auto advance = _clampedUint16(ubrk1 - ubrk0);
            const auto width = 1 + IsGlyphFullWidth({ _chars.data() + ubrk0, advance });

            ASSERT(width == expectedCols);
            ASSERT(expectedText.data() == _chars.data() + ubrk0 && expectedText.size() == advance);

            ++it;
        }

        ASSERT(it == end);
    }
#endif

    chars = { it, end };
    return col3;
}
catch (...)
{
    // Due to this function writing _charOffsets first, then calling _processUnicode/_resizeChars
    // (which may throw) and only then finally filling in _chars, we might end up
    // in a situation were _charOffsets contains offsets outside of the _chars array.
    // --> Restore this row to a known "okay"-state.
    Reset(TextAttribute{});
    throw;
}

// This function represents the slow path of ReplaceCharacters(),
// as it reallocates the backing buffer and shifts the char offsets.
// The parameters are difficult to explain, but their names are identical to
// local variables in ReplaceCharacters() which I've attempted to document there.
void ROW::_resizeChars(uint16_t colExtEnd, uint16_t chExtBeg, uint16_t chExtEnd, size_t chExtEndNew)
{
    const auto diff = chExtEndNew - chExtEnd;
    const auto currentLength = _charSize();
    const auto newLength = currentLength + diff;

    if (newLength <= _chars.size())
    {
        std::copy_n(_chars.begin() + chExtEnd, currentLength - chExtEnd, _chars.begin() + chExtEndNew);
    }
    else
    {
        const auto minCapacity = std::min<size_t>(UINT16_MAX, _chars.size() + (_chars.size() >> 1));
        const auto newCapacity = gsl::narrow<uint16_t>(std::max(newLength, minCapacity));

        auto charsHeap = std::make_unique_for_overwrite<wchar_t[]>(newCapacity);
        const std::span chars{ charsHeap.get(), newCapacity };

        std::copy_n(_chars.begin(), chExtBeg, chars.begin());
        std::copy_n(_chars.begin() + chExtEnd, currentLength - chExtEnd, chars.begin() + chExtEndNew);

        _charsHeap = std::move(charsHeap);
        _chars = chars;
    }

    auto it = _charOffsets.begin() + colExtEnd;
    const auto end = _charOffsets.end();
    for (; it != end; ++it)
    {
        *it = gsl::narrow_cast<uint16_t>(*it + diff);
    }
}

const til::small_rle<TextAttribute, uint16_t, 1>& ROW::Attributes() const noexcept
{
    return _attr;
}

TextAttribute ROW::GetAttrByColumn(const til::CoordType column) const
{
    return _attr.at(_clampedUint16(column));
}

std::vector<uint16_t> ROW::GetHyperlinks() const
{
    std::vector<uint16_t> ids;
    for (const auto& run : _attr.runs())
    {
        if (run.value.IsHyperlink())
        {
            ids.emplace_back(run.value.GetHyperlinkId());
        }
    }
    return ids;
}

uint16_t ROW::size() const noexcept
{
    return _columnCount;
}

til::CoordType ROW::MeasureLeft() const noexcept
{
    const auto text = GetText();
    const auto beg = text.begin();
    const auto end = text.end();
    auto it = beg;

    for (; it != end; ++it)
    {
        if (*it != L' ')
        {
            break;
        }
    }

    return gsl::narrow_cast<til::CoordType>(it - beg);
}

til::CoordType ROW::MeasureRight() const noexcept
{
    const auto text = GetText();
    const auto beg = text.begin();
    const auto end = text.end();
    auto it = end;

    for (; it != beg; --it)
    {
        // it[-1] is safe as `it` is always greater than `beg` (loop invariant).
        if (til::at(it, -1) != L' ')
        {
            break;
        }
    }

    // We're supposed to return the measurement in cells and not characters
    // and therefore simply calculating `it - beg` would be wrong.
    //
    // An example: The row is 10 cells wide and `it` points to the second character.
    // `it - beg` would return 1, but it's possible it's actually 1 wide glyph and 8 whitespace.
    return gsl::narrow_cast<til::CoordType>(_columnCount - (end - it));
}

bool ROW::ContainsText() const noexcept
{
    const auto text = GetText();
    const auto beg = text.begin();
    const auto end = text.end();
    auto it = beg;

    for (; it != end; ++it)
    {
        if (*it != L' ')
        {
            return true;
        }
    }

    return false;
}

std::wstring_view ROW::GlyphAt(til::CoordType column) const noexcept
{
    auto col = _clampedColumn(column);

    // Safety: col is [0, _columnCount).
    const auto beg = _uncheckedCharOffset(col);
    // Safety: col cannot be incremented past _columnCount, because the last
    // _charOffset at index _columnCount will never get the CharOffsetsTrailer flag.
    while (_uncheckedIsTrailer(++col))
    {
    }
    // Safety: col is now (0, _columnCount].
    const auto end = _uncheckedCharOffset(col);

    return { _chars.begin() + beg, _chars.begin() + end };
}

DbcsAttribute ROW::DbcsAttrAt(til::CoordType column) const noexcept
{
    const auto col = _clampedColumn(column);

    auto attr = DbcsAttribute::Single;
    // Safety: col is [0, _columnCount).
    if (_uncheckedIsTrailer(col))
    {
        attr = DbcsAttribute::Trailing;
    }
    // Safety: col+1 is [1, _columnCount].
    else if (_uncheckedIsTrailer(::base::strict_cast<size_t>(col) + 1u))
    {
        attr = DbcsAttribute::Leading;
    }

    return { attr };
}

std::wstring_view ROW::GetText() const noexcept
{
    return { _chars.data(), _charSize() };
}

DelimiterClass ROW::DelimiterClassAt(til::CoordType column, const std::wstring_view& wordDelimiters) const noexcept
{
    const auto col = _clampedColumn(column);
    // Safety: col is [0, _columnCount).
    const auto glyph = _uncheckedChar(_uncheckedCharOffset(col));

    if (glyph <= L' ')
    {
        return DelimiterClass::ControlChar;
    }
    else if (wordDelimiters.find(glyph) != std::wstring_view::npos)
    {
        return DelimiterClass::DelimiterChar;
    }
    else
    {
        return DelimiterClass::RegularChar;
    }
}

template<typename T>
constexpr uint16_t ROW::_clampedUint16(T v) noexcept
{
    return static_cast<uint16_t>(std::max(T{ 0 }, std::min(T{ 65535 }, v)));
}

template<typename T>
constexpr uint16_t ROW::_clampedColumn(T v) const noexcept
{
    return static_cast<uint16_t>(std::max(T{ 0 }, std::min<T>(_columnCount - 1u, v)));
}

template<typename T>
constexpr uint16_t ROW::_clampedColumnInclusive(T v) const noexcept
{
    return static_cast<uint16_t>(std::max(T{ 0 }, std::min<T>(_columnCount, v)));
}

// Safety: off must be [0, _charSize()].
wchar_t ROW::_uncheckedChar(size_t off) const noexcept
{
    return til::at(_chars, off);
}

uint16_t ROW::_charSize() const noexcept
{
    // Safety: _charOffsets is an array of `_columnCount + 1` entries.
    return til::at(_charOffsets, _columnCount);
}

// Safety: col must be [0, _columnCount].
uint16_t ROW::_uncheckedCharOffset(size_t col) const noexcept
{
    assert(col < _charOffsets.size());
    return til::at(_charOffsets, col) & CharOffsetsMask;
}

// Safety: col must be [0, _columnCount].
bool ROW::_uncheckedIsTrailer(size_t col) const noexcept
{
    assert(col < _charOffsets.size());
    return WI_IsFlagSet(til::at(_charOffsets, col), CharOffsetsTrailer);
}
