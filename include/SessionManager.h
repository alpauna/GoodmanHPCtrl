#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <Arduino.h>

struct Session {
    char token[33];              // 32 hex chars + null (128 bits via esp_fill_random)
    unsigned long lastActivity;  // millis() of last valid request
    bool active;                 // Slot in use
};

class SessionManager {
public:
    SessionManager();

    String createSession(const String& clientIP);
    bool validateSession(const String& token);  // Also refreshes lastActivity
    void invalidateSession(const String& token);

    void setTimeoutMinutes(uint32_t minutes) { _timeoutMinutes = minutes; }
    uint32_t getTimeoutMinutes() const { return _timeoutMinutes; }
    bool isEnabled() const { return _timeoutMinutes > 0; }
    uint8_t getActiveSessionCount() const;

    static String extractSessionToken(const String& cookieHeader);

private:
    void cleanup();  // Remove expired sessions

    static constexpr uint8_t MAX_SESSIONS = 5;
    Session _sessions[MAX_SESSIONS];
    uint32_t _timeoutMinutes = 0;  // 0 = disabled (legacy Basic Auth)
};

#endif
