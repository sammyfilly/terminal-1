/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- cmdline.h

Abstract:
- This file contains the internal structures and definitions used by command line input and editing.

Author:
- Therese Stowell (ThereseS) 15-Nov-1991

Revision History:
- Mike Griese (migrie) Jan 2018:
    Refactored the history and alias functionality into their own files.
- Michael Niksa (miniksa) May 2018:
    Split apart popup information. Started encapsulating command line things. Removed 0 length buffers.
Notes:
    The input model for the command line editing popups is complex.
    Here is the relevant pseudocode:

    CookedReadWaitRoutine
        if (CookedRead->Popup)
            Status = (*CookedRead->Popup->Callback)();
            if (Status == CONSOLE_STATUS_READ_COMPLETE)
                return STATUS_SUCCESS;
            return Status;

    CookedRead
        if (Command Line Editing Key)
            ProcessCommandLine
        else
            process regular key

    ProcessCommandLine
        if F7
            return Popup

    Popup
        draw popup
        return ProcessCommandListInput

    ProcessCommandListInput
        while (TRUE)
            GetChar
            if (wait)
                return wait
            switch (char)
                .
                .
                .
--*/

#pragma once

#include "input.h"
#include "screenInfo.hpp"
#include "server.h"

#include "history.h"
#include "alias.h"
#include "readDataCooked.hpp"
#include "popup.h"

class CommandLine final
{
public:
    static CommandLine& Instance();

    static bool IsEditLineEmpty();
    void Hide(const bool fUpdateFields);
    void Show();
    bool IsVisible() const noexcept;

    [[nodiscard]] NTSTATUS ProcessCommandLine(COOKED_READ_DATA& cookedReadData, _In_ WCHAR wch, const DWORD dwKeyState);
    [[nodiscard]] HRESULT StartCommandNumberPopup(COOKED_READ_DATA& cookedReadData);

    bool HasPopup() const noexcept;
    Popup& GetPopup() const;

    void EndCurrentPopup();
    void EndAllPopups();

    void DeletePromptAfterCursor(COOKED_READ_DATA& cookedReadData) noexcept;
    til::point DeleteFromRightOfCursor(COOKED_READ_DATA& cookedReadData) noexcept;

protected:
    CommandLine();

    // delete these because we don't want to accidentally get copies of the singleton
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;

#ifdef UNIT_TESTING
    friend class CommandLineTests;
    friend class CommandNumberPopupTests;
#endif

private:
    std::deque<std::unique_ptr<Popup>> _popups;
    bool _isVisible;
};

void DeleteCommandLine(COOKED_READ_DATA& cookedReadData, const bool fUpdateFields);

void RedrawCommandLine(COOKED_READ_DATA& cookedReadData);

// Values for WriteChars(), WriteCharsLegacy() dwFlags
#define WC_INTERACTIVE 0x01
#define WC_KEEP_CURSOR_VISIBLE 0x02

// Word delimiters
bool IsWordDelim(const wchar_t wch);
bool IsWordDelim(const std::wstring_view charData);

bool IsValidStringBuffer(_In_ bool Unicode, _In_reads_bytes_(Size) PVOID Buffer, _In_ ULONG Size, _In_ ULONG Count, ...);

void SetCurrentCommandLine(COOKED_READ_DATA& cookedReadData, _In_ SHORT Index);
