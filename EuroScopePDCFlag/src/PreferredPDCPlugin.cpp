#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../SDK/EuroScopePlugIn.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
static std::string g_lastRenderedCallsign;
static ULONGLONG g_lastRenderedTick = 0;

using namespace EuroScopePlugIn;

namespace
{

    constexpr int TAG_PDC_FLAG_TYPE = 9101;
    constexpr int FUNC_PDC_GENERATE = 9201;

    std::string Upper(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c)
                       { return (char)std::toupper(c); });
        return s;
    }

    const char *MonthShort(int mon0)
    {
        static const char *m[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        if (mon0 < 0 || mon0 > 11)
            return "UNK";
        return m[mon0];
    }

    std::string TimestampDDMonYY_HHMM()
    {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_s(&utc, &t);

        int yy = (utc.tm_year + 1900) % 100;
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << utc.tm_mday
            << MonthShort(utc.tm_mon)
            << std::setw(2) << yy
            << " " << std::setfill('0') << std::setw(2) << utc.tm_hour
            << std::setw(2) << utc.tm_min;
        return oss.str();
    }

    std::string GeneratePdcIdentifier(const std::string &callsign)
    {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::hash<std::string> hashFn;
        unsigned int seedInt = (unsigned int)hashFn(callsign);

        unsigned int pdcNumbers = ((unsigned int)now + seedInt) % 900 + 100; // 100-999
        char letter = 'A';
        letter += (((unsigned int)now + seedInt) % 25);

        std::ostringstream oss;
        oss << pdcNumbers << letter;
        return oss.str();
    }

    bool SendToBridge(const std::string &callsign, const std::string &pdc, std::string &errOut)
    {
        const char *pipeName = R"(\\.\pipe\EuroScopePDCBridge)";
        HANDLE h = CreateFileA(pipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            errOut = "Could not connect to PDCBridge (is it running?).";
            return false;
        }

        std::string payload;
        payload.reserve(callsign.size() + pdc.size() + 80);

        payload += "{\"callsign\":\"";
        for (char c : callsign)
        {
            if (c == '"' || c == '\\')
                payload += '\\';
            payload += c;
        }
        payload += "\",\"pdc\":\"";
        for (char c : pdc)
        {
            if (c == '"' || c == '\\')
                payload += '\\';
            if (c == '\n')
            {
                payload += "\\n";
                continue;
            }
            if (c == '\r')
                continue;
            payload += c;
        }
        payload += "\"}\n";

        DWORD written = 0;
        BOOL ok = WriteFile(h, payload.data(), (DWORD)payload.size(), &written, nullptr);
        CloseHandle(h);

        if (!ok || written != payload.size())
        {
            errOut = "Failed writing to PDCBridge pipe.";
            return false;
        }
        return true;
    }
} 

constexpr ULONGLONG LAST_RENDERED_FALLBACK_MS = 1500ULL;

class CPreferredPDCPlugin final : public CPlugIn
{
public:
    CPreferredPDCPlugin()
        : CPlugIn(COMPATIBILITY_CODE, "PreferredPDCFlag", "1.0.0", "Charlie", "Preferred PDC Flag")
    {
        RegisterTagItemType("PDC Flag", TAG_PDC_FLAG_TYPE);
        RegisterTagItemFunction("Generate PDC (bridge)", FUNC_PDC_GENERATE);

        DisplayUserMessage(
            "PreferredPDCFlag", "OK",
            "Loaded. Departure List: add a column with Tag Item Type='PDC Flag' and Function='Generate PDC (bridge)'. "
            "IMPORTANT: set column WIDTH to 1-2 chars so it looks like a checkbox. "
            "Run tools\\PDCBridge\\PDCBridge.exe first.",
            true, true, false, false, false);
    }

    ~CPreferredPDCPlugin() override = default;

    void OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget, int ItemCode, int,
                      char sItemString[16], int *pColorCode, COLORREF *pRGB, double *) override
    {
        if (ItemCode != TAG_PDC_FLAG_TYPE)
            return;

        CacheFp(FlightPlan);

        std::string cs = Upper(FlightPlan.GetCallsign());
        if (cs.empty())
        {
            strncpy_s(sItemString, 16, "", _TRUNCATE);
            if (pColorCode)
                *pColorCode = TAG_COLOR_DEFAULT;
            return;
        }
        g_lastRenderedCallsign = cs;
        g_lastRenderedTick = GetTickCount64();

        const bool done = (m_pdcDone.find(cs) != m_pdcDone.end());

        std::string encoded;
        encoded.reserve(2 + cs.size());
        encoded += (done ? 'X' : '.');
        encoded += '|';
        encoded += cs;

    
        strncpy_s(sItemString, 16, encoded.c_str(), _TRUNCATE);

        if (pColorCode)
        {
            if (done && pRGB)
            {
                *pColorCode = TAG_COLOR_RGB_DEFINED;
                *pRGB = RGB(0, 200, 0);
            }
            else
            {
                *pColorCode = TAG_COLOR_DEFAULT;
            }
        }
    }

    void OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area) override
    {
        if (FunctionId != FUNC_PDC_GENERATE)
            return;

        std::string raw = sItemString ? sItemString : "";
        std::string cs;

        const auto bar = raw.find('|');
        if (bar != std::string::npos && bar + 1 < raw.size())
            cs = raw.substr(bar + 1);

        cs = Upper(cs);


        if (cs.empty())
        {
            CFlightPlan fpASEL = FlightPlanSelectASEL();
            if (fpASEL.IsValid())
            {
                cs = Upper(fpASEL.GetCallsign());
            }
            else
            {
                ULONGLONG now = GetTickCount64();
                if (!g_lastRenderedCallsign.empty() && (now - g_lastRenderedTick) < LAST_RENDERED_FALLBACK_MS)
                {
                    cs = g_lastRenderedCallsign;
                }
            }
        }

        if (cs.empty())
        {
            DisplayUserMessage(
                "PreferredPDCFlag", "ERROR",
                "Could not determine callsign from click. Click directly on the PDC cell (not just the header), or ensure the PDC column is visible.",
                true, true, false, false, false);
            return;
        }

        CFlightPlan fp = FlightPlanSelect(cs.c_str());
        if (!fp.IsValid())
        {
            DisplayUserMessage("PreferredPDCFlag", "ERROR",
                               ("FlightPlanSelect failed for " + cs).c_str(),
                               true, true, false, false, false);
            return;
        }

        CacheFp(fp);

        auto it = m_lastFpCache.find(cs);
        if (it == m_lastFpCache.end())
        {
            DisplayUserMessage("PreferredPDCFlag", "ERROR",
                               ("No cached data yet for " + cs + ". Wait 1 second and click again.").c_str(),
                               true, true, false, false, false);
            return;
        }

        const CachedFp &cfp = it->second;

        const std::string pdcId = GeneratePdcIdentifier(cs);
        const std::string pdc = cfp.BuildPDCText(pdcId);

        fp.GetControllerAssignedData().SetScratchPadString(pdcId.c_str());

        std::string err;
        if (!SendToBridge(cs, pdc, err))
        {
            DisplayUserMessage("PreferredPDCFlag", "ERROR", err.c_str(), true, true, false, false, false);
            return;
        }

        m_pdcDone.insert(cs);

        DisplayUserMessage("PreferredPDCFlag", "OK",
                           ("PDC prepared for " + cs + " (ID " + pdcId + "). Bridge will open private chat + paste; you press Enter.").c_str(),
                           true, true, false, false, false);
    }

    void OnFlightPlanDisconnect(CFlightPlan FlightPlan) override
    {
        std::string cs = Upper(FlightPlan.GetCallsign());
        if (!cs.empty())
        {
            m_pdcDone.erase(cs);
            m_lastFpCache.erase(cs);
        }
    }

    void OnFlightPlanFlightPlanDataUpdate(CFlightPlan FlightPlan) override { CacheFp(FlightPlan); }
    void OnFlightPlanControllerAssignedDataUpdate(CFlightPlan FlightPlan, int) override { CacheFp(FlightPlan); }

private:
    struct CachedFp
    {
        std::string callsign, dep, dest, sid, rwy, squawk, actype, alt, route;

        std::string BuildPDCText(const std::string &pdcId) const
        {
            std::ostringstream out;
            out
                << "TIMESTAMP " << TimestampDDMonYY_HHMM() << " "
                << "*PRE-DEPARTURE CLEARANCE* "
                << "FLT " << callsign << " " << dep << " "
                << actype << " FILED " << alt << " "
                << "XPRD " << squawk << " "
                << "USE SID " << sid << " "
                << "DEPARTURE RUNWAY " << rwy << " "
                << "DESTINATION " << dest << " "
                << "*** ADVISE ATC IF RUNUP REQUIRED *** "
                << "CONTACT CLEARANCE WITH IDENTIFIER " << pdcId << " "
                << route << " "
                << "END";
            return out.str();
        }
    };
    std::vector<std::string> m_depListOrder;

    void CacheFp(CFlightPlan fp)
    {
        if (!fp.IsValid())
            return;

        CachedFp c{};
        c.callsign = Upper(fp.GetCallsign());
        if (c.callsign.empty())
            return;

        const auto fpd = fp.GetFlightPlanData();
        const auto cad = fp.GetControllerAssignedData();

        c.dep = Upper(fpd.GetOrigin());
        c.dest = Upper(fpd.GetDestination());
        c.sid = Upper(fpd.GetSidName());
        c.rwy = Upper(fpd.GetDepartureRwy());
        c.squawk = cad.GetSquawk();
        c.actype = Upper(fpd.GetAircraftFPType());
        c.route = fpd.GetRoute();

        const int fa = fpd.GetFinalAltitude();
        if (fa > 18000)
            c.alt = "FL" + std::to_string(fa / 100);
        else
            c.alt = std::to_string(fa);

        m_lastFpCache[c.callsign] = std::move(c);
    }

    std::unordered_set<std::string> m_pdcDone;
    std::unordered_map<std::string, CachedFp> m_lastFpCache;
};

static CPreferredPDCPlugin *g_plugin = nullptr;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn **ppPlugInInstance)
{
    if (!ppPlugInInstance)
        return;
    g_plugin = new CPreferredPDCPlugin();
    *ppPlugInInstance = g_plugin;
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
