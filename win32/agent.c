/*
 * Client code to connect to Windows sh-agent service (openssh-portable).
 *
 * Based on weasel-pageant, Copyright 2017 Valtteri Vuorikoski which is based on ssh-pageant, Copyright 2009, 2011  Josh Stone
 *
 * This file is part of ssh-agent-wsl, and is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This file is derived from part of the PuTTY program, whose original
 * license is available in COPYING.PuTTY.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

#include "../common.h"
#include "agent.h"

#define AGENT_PIPE_ID L"\\\\.\\pipe\\openssh-ssh-agent"
#define SSH_AGENT_FAILURE ERROR_ACCESS_DENIED

uint32_t flags = 0;

// Rupor: printing debug output does not always work, probably due to buffering and WSL/WIN32 interoperability, so
// we'll use proper OutputDebugString here
void print_debug(const char *fmt, ...)
{
    if (!(flags & WSLP_CHILD_FLAG_DEBUG))
        return;

    char buffer[1024];
    strcpy(buffer, "ssh-agent-wsl (Win32): ");

    va_list ap;

    va_start(ap, fmt);
    vsprintf(buffer+strlen(buffer), fmt, ap);
    OutputDebugStringA(buffer);
    va_end(ap);
}

static PSID get_user_sid(void)
{
    HANDLE      proc = NULL, tok = NULL;
    TOKEN_USER* user = NULL;
    DWORD       toklen, sidlen;
    PSID        sid = NULL, ret = NULL;

    if ((proc = OpenProcess(MAXIMUM_ALLOWED, FALSE, GetCurrentProcessId())) && OpenProcessToken(proc, TOKEN_QUERY, &tok) &&
        (!GetTokenInformation(tok, TokenUser, NULL, 0, &toklen) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) && (user = (TOKEN_USER*)LocalAlloc(LPTR, toklen)) &&
        GetTokenInformation(tok, TokenUser, user, toklen, &toklen)) {
        sidlen = GetLengthSid(user->User.Sid);
        sid    = (PSID)malloc(sidlen);
        if (sid && CopySid(sidlen, sid, user->User.Sid)) {
            /* Success. Move sid into the return value slot, and null it out
             * to stop the cleanup code freeing it. */
            ret = sid;
            sid = NULL;
        }
    }

    if (proc != NULL)
        CloseHandle(proc);
    if (tok != NULL)
        CloseHandle(tok);
    LocalFree(user);
    free(sid);

    return ret;
}

void agent_query(void* buf)
{
    static const char reply_error[5] = {0, 0, 0, 1, SSH_AGENT_FAILURE};

    PSECURITY_DESCRIPTOR psd = NULL;
    SECURITY_ATTRIBUTES  sa, *psa = NULL;
    PSID                 usersid = get_user_sid();
    if (usersid) {
        psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
        if (psd) {
            if (InitializeSecurityDescriptor(psd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorOwner(psd, usersid, FALSE)) {
                sa.nLength              = sizeof(sa);
                sa.bInheritHandle       = TRUE;
                sa.lpSecurityDescriptor = psd;
                psa                     = &sa;
            }
        }
    }

    HANDLE hPipe;
    while (1) {

        hPipe = CreateFile(AGENT_PIPE_ID, GENERIC_READ | GENERIC_WRITE, 0, psa, OPEN_EXISTING, 0, NULL);

        // Break if we have it
        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        // Exit if an error other than ERROR_PIPE_BUSY occurs.
        if (GetLastError() != ERROR_PIPE_BUSY) {
            print_debug("Can't open pipe: %d", GetLastError());
            memcpy(buf, reply_error, msglen(reply_error));
            return;
        }

        // All pipe instances are busy, so wait for 1 second.
        if (!WaitNamedPipe(AGENT_PIPE_ID, 1000)) {
            memcpy(buf, reply_error, msglen(reply_error));
            return;
        }
    }

    print_debug("agent_query connected to the pipe");

    DWORD cbWritten;
    if (!WriteFile(hPipe, buf, msglen(buf), &cbWritten, NULL)) {
        print_debug("Can't write to pipe: %d", GetLastError());
        memcpy(buf, reply_error, msglen(reply_error));
        return;
    }

    DWORD cbRead;
    BOOL  fSuccess = FALSE;
    do {
        fSuccess = ReadFile(hPipe, buf, AGENT_MAX_MSGLEN, &cbRead, NULL);
        if (!fSuccess && GetLastError() != ERROR_MORE_DATA)
            break;
    } while (!fSuccess);

    if (!fSuccess) {
        print_debug("Can't read from pipe: %d", GetLastError());
        memcpy(buf, reply_error, msglen(reply_error));
        return;
    }

    CloseHandle(hPipe);
    print_debug("agent_query done");
}

