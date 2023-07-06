// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "CopyFromCharPopup.hpp"

#include "_stream.h"
#include "resource.h"

static constexpr size_t COPY_FROM_CHAR_PROMPT_LENGTH = 28;

CopyFromCharPopup::CopyFromCharPopup(SCREEN_INFORMATION& screenInfo) :
    Popup(screenInfo, { COPY_FROM_CHAR_PROMPT_LENGTH + 2, 1 })
{
}

// Routine Description:
// - This routine handles the delete from cursor to char popup.  It returns when we're out of input or the user has entered a char.
// Return Value:
// - CONSOLE_STATUS_WAIT - we ran out of input, so a wait block was created
// - CONSOLE_STATUS_READ_COMPLETE - user hit return
[[nodiscard]] NTSTATUS CopyFromCharPopup::Process(COOKED_READ_DATA& cookedReadData) noexcept
{
    // get user input
    auto Char = UNICODE_NULL;
    auto PopupKeys = false;
    DWORD modifiers = 0;
    auto Status = _getUserInput(cookedReadData, PopupKeys, modifiers, Char);
    if (FAILED_NTSTATUS(Status))
    {
        return Status;
    }

    CommandLine::Instance().EndCurrentPopup();

    if (PopupKeys && Char == VK_ESCAPE)
    {
        return CONSOLE_STATUS_WAIT_NO_BLOCK;
    }

    return CONSOLE_STATUS_WAIT_NO_BLOCK;
}

void CopyFromCharPopup::_DrawContent()
{
    _DrawPrompt(ID_CONSOLE_MSGCMDLINEF4);
}
