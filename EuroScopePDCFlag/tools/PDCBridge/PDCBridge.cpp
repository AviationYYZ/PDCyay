#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <string>
#include <sstream>

// Very small JSON-ish parser for: {"callsign":"...","pdc":"..."}\n
static bool ExtractField(const std::string &s, const char *key, std::string &out)
{
    std::string k = std::string("\"") + key + "\":\"";
    auto p = s.find(k);
    if (p == std::string::npos)
        return false;
    p += k.size();
    std::string v;
    while (p < s.size())
    {
        char c = s[p++];
        if (c == '\\')
        {
            if (p >= s.size())
                break;
            char n = s[p++];
            if (n == 'n')
                v.push_back('\n');
            else
                v.push_back(n);
        }
        else if (c == '"')
        {
            break;
        }
        else
        {
            v.push_back(c);
        }
    }
    out = v;
    return true;
}

static void CopyToClipboard(const std::string &text)
{
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!h)
    {
        CloseClipboard();
        return;
    }

    char *p = (char *)GlobalLock(h);
    memcpy(p, text.c_str(), text.size() + 1);
    GlobalUnlock(h);

    SetClipboardData(CF_TEXT, h);
    CloseClipboard();
}

// Types into the focused window (EuroScope) without pressing Enter.
static void TypeText(const std::string &text)
{
    for (char ch : text)
    {
        SHORT vk = VkKeyScanA(ch);
        BYTE vkCode = LOBYTE(vk);
        BYTE shift = HIBYTE(vk);

        // Handle shift
        if (shift & 1)
            keybd_event(VK_SHIFT, 0, 0, 0);

        keybd_event((BYTE)vkCode, 0, 0, 0);
        keybd_event((BYTE)vkCode, 0, KEYEVENTF_KEYUP, 0);

        if (shift & 1)
            keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);

        Sleep(2);
    }
}

int main()
{
    const char *pipeName = R"(\\.\pipe\EuroScopePDCBridge)";
    printf("PDCBridge running. Pipe: %s\n", pipeName);
    printf("Keep EuroScope focused when you click the PDC flag.\n\n");

    for (;;)
    {
        HANDLE hPipe = CreateNamedPipeA(
            pipeName,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            printf("CreateNamedPipe failed.\n");
            return 1;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected)
        {
            CloseHandle(hPipe);
            continue;
        }

        std::string buf;
        buf.resize(8192);
        DWORD read = 0;
        if (ReadFile(hPipe, buf.data(), (DWORD)buf.size() - 1, &read, nullptr) && read > 0)
        {
            buf[read] = 0;
            std::string line = buf.c_str();

            std::string callsign, pdc;
            if (ExtractField(line, "callsign", callsign) && ExtractField(line, "pdc", pdc))
            {
                // Always use the NEW callsign from the message.
                CopyToClipboard(pdc);

                // Open private chat and paste into it.
                // You MUST configure this command to match how YOU open private chat in EuroScope.
                // Common pattern: ".chat <CALLSIGN>" OR ".msg <CALLSIGN>"
                std::string openCmd = ".chat " + callsign; // <-- change if your setup uses a different command

                // Type open command, press Enter, then Ctrl+V (but do NOT press Enter again)
                TypeText(openCmd);
                keybd_event(VK_RETURN, 0, 0, 0);
                keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);

                Sleep(80);

                // Ctrl+V
                keybd_event(VK_CONTROL, 0, 0, 0);
                keybd_event('V', 0, 0, 0);
                keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
                keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
                Sleep(50); // small delay to ensure paste completes

                // Press Enter to send the message
                keybd_event(VK_RETURN, 0, 0, 0);
                keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);

                printf("OK: %s PDC sent\n", callsign.c_str());
            }
            else
            {
                printf("Bad payload: %s\n", line.c_str());
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}
