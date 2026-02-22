#include "SessionManager.h"
#include "esp_random.h"

SessionManager::SessionManager() {
    memset(_sessions, 0, sizeof(_sessions));
}

String SessionManager::createSession(const String& clientIP) {
    cleanup();

    // Find a free slot (or evict oldest)
    int slot = -1;
    unsigned long oldest = ULONG_MAX;
    int oldestSlot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!_sessions[i].active) {
            slot = i;
            break;
        }
        if (_sessions[i].lastActivity < oldest) {
            oldest = _sessions[i].lastActivity;
            oldestSlot = i;
        }
    }
    if (slot < 0) slot = oldestSlot;  // Evict oldest

    // Generate 128-bit random token as 32 hex chars
    uint8_t rng[16];
    esp_fill_random(rng, sizeof(rng));
    for (int i = 0; i < 16; i++) {
        snprintf(_sessions[slot].token + i * 2, 3, "%02x", rng[i]);
    }
    _sessions[slot].token[32] = '\0';
    _sessions[slot].lastActivity = millis();
    _sessions[slot].active = true;

    return String(_sessions[slot].token);
}

bool SessionManager::validateSession(const String& token) {
    if (token.length() != 32) return false;
    cleanup();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active && token.equals(_sessions[i].token)) {
            _sessions[i].lastActivity = millis();
            return true;
        }
    }
    return false;
}

void SessionManager::invalidateSession(const String& token) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active && token.equals(_sessions[i].token)) {
            _sessions[i].active = false;
            _sessions[i].token[0] = '\0';
            return;
        }
    }
}

void SessionManager::cleanup() {
    if (_timeoutMinutes == 0) return;
    unsigned long now = millis();
    unsigned long timeoutMs = (unsigned long)_timeoutMinutes * 60UL * 1000UL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active && (now - _sessions[i].lastActivity) >= timeoutMs) {
            _sessions[i].active = false;
            _sessions[i].token[0] = '\0';
        }
    }
}

uint8_t SessionManager::getActiveSessionCount() const {
    uint8_t count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active) count++;
    }
    return count;
}

String SessionManager::extractSessionToken(const String& cookieHeader) {
    // Parse "session=<token>" from Cookie header (may contain multiple cookies)
    int idx = cookieHeader.indexOf("session=");
    if (idx < 0) return "";
    int start = idx + 8;  // strlen("session=")
    int end = cookieHeader.indexOf(';', start);
    if (end < 0) end = cookieHeader.length();
    return cookieHeader.substring(start, end);
}
