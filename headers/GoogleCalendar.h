#pragma once
#include <string>
#include <vector>
#include <ctime>

using namespace std;

struct Meeting {
    string uuid;
    string name;
    time_t startTimestamp;
    string startTimeISO;// ISO 8601 string (debug)
    bool isBooking = false;
    string bookedBy;
};

struct OAuthTokenResult {
    bool success = false;
    string accessToken;
    string refreshToken; // empty if not returned by server
    int expiresInSec = 0;
    string errorMessage;
};

OAuthTokenResult RunOAuthLoginFlow(const string& clientId, const string& clientSecret); // starts the OAuth login flow, returns access token and refresh token if successful
OAuthTokenResult RefreshAccessToken(const string& clientId, const string& clientSecret, const string& refreshToken);
bool EnsureValidAccessToken(const string& clientId, const string& clientSecret, const string& refreshToken, string& ioAccessToken, time_t& ioExpiry);
bool FetchUpcomingMeetings(const string& accessToken, const string& calendarId, vector<Meeting>& outMeetings); // fetches upcoming meetings from the specified calendar, returns false on error

