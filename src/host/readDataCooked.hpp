// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "readData.hpp"

class COOKED_READ_DATA final : public ReadData
{
public:
    COOKED_READ_DATA(_In_ InputBuffer* const pInputBuffer,
                     _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                     SCREEN_INFORMATION& screenInfo,
                     _In_ size_t UserBufferSize,
                     _In_ char* UserBuffer,
                     _In_ ULONG CtrlWakeupMask,
                     _In_ const std::wstring_view exeName,
                     _In_ const std::wstring_view initialData,
                     _In_ ConsoleProcessHandle* const pClientProcess);

    ~COOKED_READ_DATA() override;

    void MigrateUserBuffersOnTransitionToBackgroundWait(const void* oldBuffer, void* newBuffer) noexcept override;

    bool Notify(const WaitTerminationReason TerminationReason,
                const bool fIsUnicode,
                _Out_ NTSTATUS* const pReplyStatus,
                _Out_ size_t* const pNumBytes,
                _Out_ DWORD* const pControlKeyState,
                _Out_ void* const pOutputData) noexcept override;

    bool Read(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState);

private:
    bool _readCharInputLoop(bool isUnicode, size_t& numBytes);
    void _handlePostCharInputLoop(bool isUnicode, size_t& numBytes, ULONG& controlKeyState);
    bool _processInput(const wchar_t wch, const DWORD keyState);

    std::span<char> _userBuffer;
    std::wstring _buffer;
    ULONG _ctrlWakeupMask;
    int _controlKeyState;
};
