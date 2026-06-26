#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <random>
#include "GoogleCalendar.h"

using namespace std;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

static const wchar_t* OAUTH_HOST = L"oauth2.googleapis.com";
static const wchar_t* OAUTH_AUTH_HOST = L"accounts.google.com";
static const char* OAUTH_SCOPE = "https://www.googleapis.com/auth/calendar.readonly";
static const int LOGIN_TIMEOUT_SEC = 180; // maximum time to wait for the user to complete the login in the browser

static string JsonGetString(const string& json, const string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == string::npos)
        return "";

    size_t colon = json.find(":", pos);
    if (colon == string::npos)
        return "";

    size_t startQuote = json.find("\"", colon);
    if (startQuote == string::npos)
        return "";

    size_t endQuote = json.find("\"", startQuote + 1);
    if (endQuote == string::npos)
        return "";

    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

static long JsonGetNumber(const string& json, const string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == string::npos)
        return -1;

    size_t colon = json.find(":", pos);
    if (colon == string::npos)
        return -1;

    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
        i++;

    size_t start = i;
    while (i < json.size() && (isdigit((unsigned char)json[i]) || json[i] == '-'))
        i++;

    if (i == start)
        return -1;

    return strtol(json.substr(start, i - start).c_str(), nullptr, 10);
}

static string UrlDecode(const string& value) {
    string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '%' && i + 2 < value.size() &&
            isxdigit((unsigned char)value[i+1]) && isxdigit((unsigned char)value[i+2])) {
            int hi = value[i+1];
            int lo = value[i+2];

            auto hexVal = [](int c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return 0;
            };

            out += (char)((hexVal(hi) << 4) | hexVal(lo));
            i += 2;
        } else if (value[i] == '+') {
            out += ' ';
        } else {
            out += value[i];
        }
    }
    return out;
}

static string UrlEncode(const string& value) {
    ostringstream escaped;
    escaped << hex;

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << uppercase << setw(2) << setfill('0') << (int)c << nouppercase;
        }
    }

    return escaped.str();
}

static string Trim(const string& s) {
    if (s.empty())
        return "";

    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");

    if (start == string::npos) {
        return "";
    }

    return s.substr(start, end - start + 1);
}

static bool ParseIntField(const string& s, size_t& pos, int digitsExpected, int& outVal) {
    if (pos + digitsExpected > s.size())
        return false;

    int val = 0;
    for (int i = 0; i < digitsExpected; i++) {
        char c = s[pos + i];
        if (!isdigit((unsigned char)c))
            return false;
        val = val * 10 + (c - '0');
    }

    outVal = val;
    pos += digitsExpected;
    return true;
}

static time_t ParseISO8601(const string& s) { // parse a date-time string in ISO 8601 format and return the corresponding time_t value
    if (s.empty())
        return 0;

    size_t pos = 0;
    int year = 0, mon = 0, mday = 0, hour = 0, min = 0, sec = 0;

    if (!ParseIntField(s, pos, 4, year))
        return 0;
    if (pos >= s.size() || s[pos] != '-')
        return 0;
    pos++;
    if (!ParseIntField(s, pos, 2, mon))
        return 0;
    if (pos >= s.size() || s[pos] != '-')
        return 0;
    pos++;
    if (!ParseIntField(s, pos, 2, mday))
        return 0;
    if (pos >= s.size() || s[pos] != 'T')
        return 0;
    pos++;
    if (!ParseIntField(s, pos, 2, hour))
        return 0;
    if (pos >= s.size() || s[pos] != ':')
        return 0;
    pos++;
    if (!ParseIntField(s, pos, 2, min))
        return 0;

    if (pos < s.size() && s[pos] == ':') {
        pos++;
        if (!ParseIntField(s, pos, 2, sec))
            return 0;
    }

    char tzSign = 0;
    int tzHour = 0, tzMin = 0;
    if (pos < s.size()) {
        if (s[pos] == 'Z') {
            tzSign = 0;
        } else if (s[pos] == '+' || s[pos] == '-') {
            tzSign = s[pos];
            pos++;
            if (!ParseIntField(s, pos, 2, tzHour))
                tzHour = 0;
            if (pos < s.size() && s[pos] == ':') {
                pos++;
                if (!ParseIntField(s, pos, 2, tzMin))
                    tzMin = 0;
            }
        }
    }

    struct tm tm_utc = {};
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon = mon - 1;
    tm_utc.tm_mday = mday;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min = min;
    tm_utc.tm_sec = sec;

    time_t t = _mkgmtime(&tm_utc);
    if (t == -1)
        return 0;

    if (tzSign == '+' || tzSign == '-') {
        time_t offsetSeconds = (tzHour * 3600) + (tzMin * 60);
        if (tzSign == '+')
            t -= offsetSeconds;
        else
            t += offsetSeconds;
    }

    return t;
}

static string GenerateRandomState() { // generate a random 32-character hexadecimal string to use as the state parameter in the OAuth flow
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dist(0, 15);
    static const char hexChars[] = "0123456789abcdef";
    string s;
    for (int i = 0; i < 32; i++)
        s += hexChars[dist(gen)];
    return s;
}

static bool HttpsRequest( // perform an HTTPS request using WinHTTP and return the response body and status code
    const wstring& host,
    const wstring& path,
    const wstring& method,
    const string& bearerToken,
    const string& postBody,
    const wstring& contentType,
    string& outBody,
    int& outStatus)
{
    outStatus = 0;
    outBody.clear();

    HINTERNET hSession = WinHttpOpen(L"GoogleCalendarNotifier/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!bearerToken.empty()) {
        string authHeader = "Authorization: Bearer " + bearerToken + "\r\n";
        wstring wAuthHeader(authHeader.begin(), authHeader.end());
        WinHttpAddRequestHeaders(hRequest, wAuthHeader.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!contentType.empty()) {
        wstring ctHeader = L"Content-Type: " + contentType + L"\r\n";
        WinHttpAddRequestHeaders(hRequest, ctHeader.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    LPVOID pBody;
    if (postBody.empty()) {
        pBody = WINHTTP_NO_REQUEST_DATA;
    } else {
        pBody = (LPVOID)postBody.data();
    }

    DWORD  bodyLen = (DWORD)postBody.size();

    bool bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, pBody, bodyLen, bodyLen, 0);

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, nullptr);
    }

    if (bResults) {
        DWORD dwStatus = 0;
        DWORD dwSize = sizeof(dwStatus);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatus, &dwSize, WINHTTP_NO_HEADER_INDEX);
        outStatus = (int)dwStatus;

        DWORD dwAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &dwAvailable) && dwAvailable > 0) {
            vector<char> buffer(dwAvailable);
            DWORD dwRead = 0;
            if (WinHttpReadData(hRequest, buffer.data(), dwAvailable, &dwRead)) {
                outBody.append(buffer.data(), dwRead);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bResults;
}

struct LocalServerResult { // result of the local HTTP server that listens for the OAuth redirect
    bool gotRequest = false;
    string code;
    string state;
    string error;
};

static LocalServerResult RunLoopbackListener(int port, int timeoutSec) { // run a local HTTP server on the specified port and wait for an incoming request with a timeout
    LocalServerResult result;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        result.error = "WSAStartup failed";
        return result;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        result.error = "socket() failed";
        WSACleanup();
        return result;
    }

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        result.error = "bind() failed (port in use)";
        closesocket(listenSock);
        WSACleanup();
        return result;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR) {
        result.error = "listen() failed";
        closesocket(listenSock);
        WSACleanup();
        return result;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSock, &readSet);
    timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;

    int selRes = select(0, &readSet, nullptr, nullptr, &tv);
    if (selRes <= 0) {
        result.error = "timeout waiting for browser redirect";
        closesocket(listenSock);
        WSACleanup();
        return result;
    }

    SOCKET clientSock = accept(listenSock, nullptr, nullptr);
    if (clientSock == INVALID_SOCKET) {
        result.error = "accept() failed";
        closesocket(listenSock);
        WSACleanup();
        return result;
    }

    char buf[8192] = {};
    int received = recv(clientSock, buf, sizeof(buf) - 1, 0);
    if (received > 0) {
        string req(buf, received);

        size_t getPos = req.find("GET ");
        size_t httpPos = req.find(" HTTP/");
        if (getPos != string::npos && httpPos != string::npos && httpPos > getPos) {
            string requestLine = req.substr(getPos + 4, httpPos - (getPos + 4));

            size_t qPos = requestLine.find('?');
            if (qPos != string::npos) {
                string query = requestLine.substr(qPos + 1);

                size_t pos = 0;
                while (pos < query.size()) {
                    size_t amp = query.find('&', pos);
                    string pair;
                    if (amp == string::npos) {
                        pair = query.substr(pos);
                    } else {
                        pair = query.substr(pos, amp - pos);
                    }

                    size_t eq = pair.find('=');
                    if (eq != string::npos) {
                        string key = pair.substr(0, eq);
                        string val = UrlDecode(pair.substr(eq + 1));
                        if (key == "code")
                            result.code = val;
                        else if (key == "state")
                            result.state = val;
                        else if (key == "error")
                            result.error = val;
                    }

                    if (amp == string::npos)
                        break;
                    pos = amp + 1;
                }
            }
            result.gotRequest = true;
        }
    }

    string page;

    if (result.code.empty()) {
        page = "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif;text-align:center;margin-top:60px;'>"
            "<h2>Authentication canceled or failed.</h2><p>You can close this window.</p></body></html>";
    } else {
        page = "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif;text-align:center;margin-top:60px;'>"
            "<h2>Authentication successful!</h2><p>You can close this window.</p></body></html>";
    }

    ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/html; charset=utf-8\r\n"
         << "Content-Length: " << page.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << page;
    string respStr = resp.str();
    send(clientSock, respStr.c_str(), (int)respStr.size(), 0);

    closesocket(clientSock);
    closesocket(listenSock);
    WSACleanup();
    return result;
}

OAuthTokenResult RunOAuthLoginFlow(const string& clientId, const string& clientSecret) {
    OAuthTokenResult result;

    if (Trim(clientId).empty() || Trim(clientSecret).empty()) {
        result.errorMessage = "Client ID or Client's Secret missing.";
        return result;
    }

    // ask the OS to assign a free ephemeral port by binding on port 0 then read back what port was actually assigned via getsockname()
    int chosenPort = -1;
    {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            result.errorMessage = "WSAStartup failed when picking a port.";
            return result;
        }

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            WSACleanup();
            result.errorMessage = "socket() failed when picking a port.";
            return result;
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0); // 0 tells the OS to pick any free port
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(s);
            WSACleanup();
            result.errorMessage = "Did not find an available port for the local HTTP server.";
            return result;
        }

        sockaddr_in bound = {};
        int boundLen = sizeof(bound);
        if (getsockname(s, (sockaddr*)&bound, &boundLen) == 0) {
            chosenPort = ntohs(bound.sin_port);
        }

        closesocket(s);
        WSACleanup();
    }

    if (chosenPort <= 0) {
        result.errorMessage = "Did not find an available port for the local HTTP server.";
        return result;
    }

    string redirectUri = "http://127.0.0.1:" + to_string(chosenPort) + "/";
    string state = GenerateRandomState();

    ostringstream authUrl;
    authUrl << "https://accounts.google.com/o/oauth2/v2/auth"
            << "?client_id=" << UrlEncode(clientId)
            << "&redirect_uri=" << UrlEncode(redirectUri)
            << "&response_type=code"
            << "&scope=" << UrlEncode(OAUTH_SCOPE)
            << "&access_type=offline"
            << "&prompt=consent"
            << "&state=" << UrlEncode(state);

    string urlStr = authUrl.str();

    HINSTANCE shRes = ShellExecuteA(nullptr, "open", urlStr.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)shRes <= 32) {
        result.errorMessage = "Did not manage to open the default browser.";
        return result;
    }

    LocalServerResult lr = RunLoopbackListener(chosenPort, LOGIN_TIMEOUT_SEC);
    if (!lr.gotRequest || lr.code.empty()) {
        if (lr.error.empty()) {
            result.errorMessage = "Did not receive response from Google (timeout or cancellation).";
        } else {
            result.errorMessage = "Error: " + lr.error;
        }
        return result;
    }

    if (lr.state != state) {
        result.errorMessage = "State mismatch - possible malicious or malformed OAuth request, canceled.";
        return result;
    }

    ostringstream body;
    body << "code=" << UrlEncode(lr.code)
         << "&client_id=" << UrlEncode(clientId)
         << "&client_secret=" << UrlEncode(clientSecret)
         << "&redirect_uri=" << UrlEncode(redirectUri)
         << "&grant_type=authorization_code";

    string respBody;
    int status = 0;
    bool ok = HttpsRequest(OAUTH_HOST, L"/token", L"POST", "", body.str(),
                            L"application/x-www-form-urlencoded", respBody, status);

    if (!ok || status != 200) {
        result.errorMessage = "Token exchange failed (status " + to_string(status) + "):\n" + respBody;
        return result;
    }

    result.accessToken  = JsonGetString(respBody, "access_token");
    result.refreshToken = JsonGetString(respBody, "refresh_token");
    long exp = JsonGetNumber(respBody, "expires_in");
    if (exp > 0) {
        result.expiresInSec = (int)exp;
    } else {
        result.expiresInSec = 3600;
    }

    if (result.accessToken.empty() || result.refreshToken.empty()) {
        result.errorMessage = "Incomplete response from Google (you might have already authorized without 'prompt=consent', or the refresh_token is missing):\n" + respBody;
        return result;
    }

    result.success = true;
    return result;
}

OAuthTokenResult RefreshAccessToken(
    const string& clientId,
    const string& clientSecret,
    const string& refreshToken)
{
    OAuthTokenResult result;

    ostringstream body;
    body << "client_id=" << UrlEncode(clientId)
         << "&client_secret=" << UrlEncode(clientSecret)
         << "&refresh_token=" << UrlEncode(refreshToken)
         << "&grant_type=refresh_token";

    string respBody;
    int status = 0;
    bool ok = HttpsRequest(OAUTH_HOST, L"/token", L"POST", "", body.str(), L"application/x-www-form-urlencoded", respBody, status);

    if (!ok || status != 200) {
        result.errorMessage = "Refresh token exchange failed (status " + to_string(status) + "):\n" + respBody;
        return result;
    }

    result.accessToken = JsonGetString(respBody, "access_token");
    long exp = JsonGetNumber(respBody, "expires_in");
    if (exp > 0) {
        result.expiresInSec = (int)exp;
    } else {
        result.expiresInSec = 3600;
    }

    if (result.accessToken.empty()) {
        result.errorMessage = "Incomplete response from Google (refresh token exchange failed):\n" + respBody;
        return result;
    }

    result.success = true;
    return result;
}

bool EnsureValidAccessToken(
    const string& clientId,
    const string& clientSecret,
    const string& refreshToken,
    string& ioAccessToken,
    time_t& ioExpiry)
{
    time_t now = time(nullptr);

    if (!ioAccessToken.empty() && ioExpiry > now + 60) {
        return true;
    }

    if (refreshToken.empty())
        return false;

    OAuthTokenResult r = RefreshAccessToken(clientId, clientSecret, refreshToken);
    if (!r.success)
        return false;

    ioAccessToken = r.accessToken;
    ioExpiry = now + r.expiresInSec;
    return true;
}

bool FetchUpcomingMeetings(
    const string& accessToken,
    const string& calendarId,
    vector<Meeting>& outMeetings)
{
    outMeetings.clear();

    string cleanToken = Trim(accessToken);
    string cleanId = Trim(calendarId);

    if (cleanToken.empty() || cleanId.empty())
        return false;

    time_t now = time(nullptr);
    time_t end = now + 7 * 24 * 3600;

    auto toISO = [](time_t t) -> string {
        struct tm tm_utc;
        gmtime_s(&tm_utc, &t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        return string(buf);
    };

    string minTime = toISO(now);
    string maxTime = toISO(end);
    string pathStr = "/calendar/v3/calendars/" + UrlEncode(cleanId) + "/events" + "?timeMin=" + UrlEncode(minTime) + "&timeMax=" + UrlEncode(maxTime) + "&singleEvents=true&orderBy=startTime";
    wstring path(pathStr.begin(), pathStr.end());

    string body;
    int status = 0;
    bool ok = HttpsRequest(L"www.googleapis.com", path, L"GET", cleanToken, "", L"", body, status);

    if (!ok || status != 200) {
        static bool errorShown = false;
        if (!errorShown) {
            string errMsg = "Failed to connect to Google Calendar!\nStatus: " + to_string(status) + "\nDetails:\n" + body;
            MessageBoxA(nullptr, errMsg.c_str(), "Google API Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
            errorShown = true;
        }
        return false;
    }

    size_t itemsPos = body.find("\"items\"");
    if (itemsPos == string::npos)
        return true;

    size_t arrStart = body.find('[', itemsPos);
    if (arrStart == string::npos)
        return true;

    size_t arrEnd = body.size();
    {
        int bracketDepth = 0;
        for (size_t i = arrStart; i < body.size(); i++) {
            if (body[i] == '[') bracketDepth++;
            else if (body[i] == ']') {
                bracketDepth--;
                if (bracketDepth == 0) {
                    arrEnd = i;
                    break;
                }
            }
        }
    }

    size_t pos = arrStart;
    while (pos < arrEnd) {
        size_t objStart = body.find('{', pos);
        if (objStart == string::npos || objStart > arrEnd)
            break;

        int depth = 0;
        size_t objEnd = objStart;
        for (size_t i = objStart; i < body.size(); i++) {
            if (body[i] == '{')
                depth++;
            else if (body[i] == '}') {
                depth--;
                if (depth == 0) {
                    objEnd = i;
                    break;
                }
            }
        }

        string obj = body.substr(objStart, objEnd - objStart + 1);

        Meeting m;
        string rawSummary = JsonGetString(obj, "summary");
        if (rawSummary.empty())
            rawSummary = "Event without title";

        m.uuid = JsonGetString(obj, "id");

        {
            size_t orgKeyPos = obj.find("\"organizer\""); // get the organizer block from the JSON
            if (orgKeyPos != string::npos) {
                size_t braceOpen = obj.find('{', orgKeyPos);
                if (braceOpen != string::npos) {
                    int d = 0;
                    size_t braceClose = braceOpen;
                    for (size_t i = braceOpen; i < obj.size(); i++) {
                        if (obj[i] == '{')
                            d++;
                        else if (obj[i] == '}') {
                            d--;
                            if (d == 0) {
                                braceClose = i;
                                break;
                            }
                        }
                    }
                    string orgBlock = obj.substr(braceOpen, braceClose - braceOpen + 1);
                    string orgEmail = JsonGetString(orgBlock, "email");
                    string orgDisplay = JsonGetString(orgBlock, "displayName");

                    if (!orgEmail.empty() && orgEmail != cleanId) { // if the organizer is not the same as the calendar, it's a booking
                        m.isBooking = true;

                        size_t parenOpen = rawSummary.rfind('(');
                        size_t parenClose = rawSummary.rfind(')');
                        if (parenOpen != string::npos && parenClose != string::npos && parenClose > parenOpen) { // if the summary has the format "Event Name (Booked By)", extract the name and bookedBy
                            m.bookedBy = Trim(rawSummary.substr(parenOpen + 1, parenClose - parenOpen - 1));
                            m.name = Trim(rawSummary.substr(0, parenOpen));
                        } else { // if the summary does not have the format, use the organizer's display name or email as bookedBy
                            if (orgDisplay.empty()) {
                                m.bookedBy = orgEmail;
                            } else {
                                m.bookedBy = orgDisplay;
                            }
                            m.name = rawSummary;
                        }
                    }
                }
            }

            if (!m.isBooking) { // if the organizer is the same as the calendar, check if the summary has the format "Event Name (Booked By)" to extract bookedBy
                size_t parenOpen = rawSummary.rfind('(');
                size_t parenClose = rawSummary.rfind(')');
                if (parenOpen != string::npos && parenClose != string::npos && parenClose > parenOpen && parenClose == rawSummary.size() - 1) {
                    m.isBooking = true;
                    m.bookedBy = Trim(rawSummary.substr(parenOpen + 1, parenClose - parenOpen - 1));
                    m.name = Trim(rawSummary.substr(0, parenOpen));
                }
            }

            if (!m.isBooking) { // if it's not a booking, just use the raw summary as the name
                m.name = rawSummary;
            }
        }

        string startBlock;
        {
            size_t startKeyPos = obj.find("\"start\"");
            if (startKeyPos != string::npos) {
                size_t braceOpen = obj.find('{', startKeyPos);
                if (braceOpen != string::npos) {
                    int depth = 0;
                    size_t braceClose = braceOpen;
                    for (size_t i = braceOpen; i < obj.size(); i++) {
                        if (obj[i] == '{')
                            depth++;
                        else if (obj[i] == '}') {
                            depth--;
                            if (depth == 0) {
                                braceClose = i;
                                break;
                            }
                        }
                    }
                    startBlock = obj.substr(braceOpen, braceClose - braceOpen + 1);
                }
            }
        }

        m.startTimeISO = JsonGetString(startBlock, "dateTime");

        if (m.startTimeISO.empty()) {
            m.startTimeISO = JsonGetString(startBlock, "date");
            if (!m.startTimeISO.empty())
            {
                m.startTimeISO += "T00:00:00Z";
            }
        }

        m.startTimestamp = ParseISO8601(m.startTimeISO);

        if (!m.uuid.empty() && m.startTimestamp > 0) {
            outMeetings.push_back(m);
        }

        pos = objEnd + 1;
    }

    return true;
}

