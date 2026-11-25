// Host test for arduino/ledmatrix/problems.h (the problem list, the plain-words text and the one-hour rule for the panel
// message; ticket 23 / slice 8). Run from the repo root:
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix tools/test_problems.cpp -o <scratch>/test_problems && <scratch>/test_problems
// Also worth running now and then with the sanitizers (they catch buffer overruns and out-of-range indexes):
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Iarduino/ledmatrix tools/test_problems.cpp -o <scratch>/test_problems_asan
// The real PROBLEM_MESSAGE_AFTER_MS (one hour) is tested, so do not build this with -DPROBLEM_MESSAGE_AFTER_MS.
// Hosts, names and addresses in here are placeholders (example.com style).
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "problems.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(name) printf("%s\n", name)

static const uint32_t MIN = 60UL * 1000;
static const uint32_t HOUR = 60 * MIN;  // the real PROBLEM_MESSAGE_AFTER_MS, checked in the constants section

// ---- guard bytes around an output buffer: an overrun on either side shows up as a changed guard ----------------------
template <size_t N> struct Guard {
  unsigned char pre[32];
  char buf[N];
  unsigned char post[32];
  Guard() { memset(pre, 0xA5, sizeof pre); memset(post, 0xA5, sizeof post); memset(buf, 'Z', N); }
  bool guardsOk() const {
    for (unsigned char b : pre) if (b != 0xA5) return false;
    for (unsigned char b : post) if (b != 0xA5) return false;
    return true;
  }
  bool terminated() const { return memchr(buf, 0, N) != nullptr; }
};

// ---- problemWords: which sentence did a raw text become? ----------------------------------------------------------------
enum Tag { PASS, IDMISSING, IDINVALID, IDOTHER, IDHARDWARE, IDHARDWAREUNPROT, SIGNERMISMATCH, SIGNERUNPROT, SIGNFAIL, AUTHMISSING, AUTHMALFORMED, AUTHBAD, AUTHREVOKED, AUTHCLOCK,
           HTTPAUTH, NOTSVC, LIMIT, DOWN, ODDSTATUS, NOANSWER, DNS, CERT, HSTIMEOUT,
           TIMEOUT, REFUSED, CLOSED, NOCONN, UNREADABLE, BUSY };
static const char *tagName(Tag t) {
  static const char *n[] = {"PASS", "IDMISSING", "IDINVALID", "IDOTHER", "IDHARDWARE", "IDHARDWAREUNPROT", "SIGNERMISMATCH", "SIGNERUNPROT", "SIGNFAIL", "AUTHMISSING", "AUTHMALFORMED",
                            "AUTHBAD", "AUTHREVOKED", "AUTHCLOCK", "HTTPAUTH", "NOTSVC", "LIMIT", "DOWN", "ODDSTATUS", "NOANSWER", "DNS",
                            "CERT", "HSTIMEOUT", "TIMEOUT", "REFUSED", "CLOSED", "NOCONN", "UNREADABLE", "BUSY"};
  return n[t];
}
// One marker per sentence (case-sensitive, taken from the sentences' own wording). PASS = none of them: the text was passed through.
static Tag classify(const char *why) {
  struct M { const char *marker; Tag tag; };
  static const M m[] = {
      {"has no identity record", IDMISSING},   {"identity record is not valid", IDINVALID}, {"identity record is for another board", IDOTHER},
      {"the chip has no key", IDHARDWARE},     {"the burned key is not fully protected", IDHARDWAREUNPROT},
      {"give different answers", SIGNERMISMATCH}, {"The key in the chip is not fully protected", SIGNERUNPROT}, {"could not sign the request", SIGNFAIL},
      {"did not receive a signature", AUTHMISSING}, {"could not read the signature", AUTHMALFORMED},
      {"does not accept this board's key", AUTHBAD}, {"has revoked this board", AUTHREVOKED}, {"clock is too far off", AUTHCLOCK},
      {"refused the request", HTTPAUTH},
      {"not the data service", NOTSVC},        {"limiting requests", LIMIT},         {"is down or had an error", DOWN},
      {"unexpected status", ODDSTATUS},        {"hung up or did not answer in time", NOANSWER},         {"(DNS)", DNS},
      {"certificate could not be checked", CERT}, {"secure handshake timed out", HSTIMEOUT},
      {"The server did not answer in time.", TIMEOUT}, {"refused the connection", REFUSED},
      {"closed the connection", CLOSED},       {"Could not connect", NOCONN},        {"cannot read", UNREADABLE},
      {"busy with another request", BUSY},
  };
  for (const M &x : m) if (strstr(why, x.marker)) return x.tag;
  return PASS;
}

struct Words { std::string why, detail; Tag tag; };

// Calls problemWords with exact-size guarded buffers (160 / 80, like ProblemEntry) and checks what must hold for every input.
static Words words(const char *raw) {
  Guard<160> w; Guard<80> d;
  problemWords(raw, w.buf, sizeof w.buf, d.buf, sizeof d.buf);
  const char *shown = raw ? raw : "(null)";
  checks++;
  if (!w.guardsOk() || !d.guardsOk()) { fails++; printf("    FAIL: guard bytes overwritten for \"%s\"\n", shown); }
  checks++;
  if (!w.terminated() || !d.terminated()) { fails++; printf("    FAIL: output not NUL-terminated for \"%s\"\n", shown); return {"", "", PASS}; }
  Words r{w.buf, d.buf, PASS};
  r.tag = classify(w.buf);
  checks++;
  if (r.why.empty()) { fails++; printf("    FAIL: empty sentence for \"%s\"\n", shown); }
  if (r.tag != PASS) {
    // the plain sentence: whole sentences, never the connection library's jargon
    static const char *jargon[] = {"x509", "mbedtls", "ssl", "tls", "eof", "-0x", "e.g.", "crl", "peer", "errno", "econn",
                                   "connect failed", "v4 ", "v6 "};
    for (const char *j : jargon) {
      checks++;
      if (problemHas(w.buf, j)) { fails++; printf("    FAIL: sentence for \"%s\" contains jargon \"%s\": %s\n", shown, j, w.buf); }
    }
    checks++;
    if (r.why.back() != '.') { fails++; printf("    FAIL: sentence for \"%s\" does not end with a full stop (cut?): %s\n", shown, w.buf); }
  }
  return r;
}

static void expectTag(int line, const char *raw, Tag want) {
  Words r = words(raw);
  checks++;
  if (r.tag != want) {
    fails++;
    printf("    FAIL line %d: \"%s\" -> %s (\"%s\"), wanted %s\n", line, raw ? raw : "(null)", tagName(r.tag), r.why.c_str(), tagName(want));
  }
}
#define EXPECT(raw, tag) expectTag(__LINE__, raw, tag)

static void testProblemHas() {
  SECTION("problemHas");
  CHECK(problemHas("anything", ""));
  CHECK(problemHas("", ""));
  CHECK(!problemHas("", "a"));
  CHECK(problemHas("Handshake TIMED OUT", "timed out"));
  CHECK(problemHas("HANDSHAKE", "handshake"));
  CHECK(problemHas("timed out", "timed out"));            // the whole text is the needle
  CHECK(problemHas("xx timed out", "timed out"));         // needle at the very end
  CHECK(!problemHas("xx timed ou", "timed out"));         // needle cut short by the end of the text
  CHECK(!problemHas("time", "timed out"));                // needle longer than the text
  CHECK(problemHas("aaab", "aab"));                       // a false start must not skip the real start
  CHECK(problemHas("abcabd", "abd"));
  CHECK(!problemHas("abcabd", "abe"));
  CHECK(problemHas("caf\xC3\xA9 refused", "refused"));    // bytes above 127 must not break the case folding
  CHECK(!problemHas("caf\xC3\xA9", "cafe"));
}

static void testKindNames() {
  SECTION("problemKindName");
  CHECK(PROBLEM_KINDS == 7);
  CHECK(strcmp(problemKindName(ProblemKind::Flights), "flights") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Weather), "weather") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Headlines), "headlines") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Logo), "logo") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Clock), "clock") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Update), "update check") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Identity), "identity") == 0);
  CHECK(strcmp(problemKindName(ProblemKind::Count), "?") == 0);
  CHECK(strcmp(problemKindName((ProblemKind)200), "?") == 0);
  for (int a = 0; a < PROBLEM_KINDS; a++)
    for (int b = a + 1; b < PROBLEM_KINDS; b++) CHECK(strcmp(problemKindName((ProblemKind)a), problemKindName((ProblemKind)b)) != 0);
}

static void testConstants() {
  SECTION("constants (the real values)");
  CHECK(PROBLEM_MESSAGE_AFTER_MS == HOUR);
  CHECK(PROBLEM_FRESH_MS == 10 * MIN);
  CHECK(PROBLEM_MESSAGE_PERIOD_MS == MIN);
  CHECK(PROBLEM_MESSAGE_SHOW_MS == 5000);
}

static void testWordsBranches() {
  SECTION("problemWords: every branch");
  // device authentication: the service's reasons (live.cpp's serviceGet) and the board's own identity record (identity.cpp)
  EXPECT(PROBLEM_RAW_AUTH_MISSING, AUTHMISSING);
  EXPECT(PROBLEM_RAW_AUTH_MALFORMED, AUTHMALFORMED);
  EXPECT(PROBLEM_RAW_AUTH_BAD_SIGNATURE, AUTHBAD);
  EXPECT(PROBLEM_RAW_AUTH_REVOKED, AUTHREVOKED);
  EXPECT(PROBLEM_RAW_AUTH_CLOCK_SKEW, AUTHCLOCK);
  EXPECT(PROBLEM_RAW_ID_MISSING, IDMISSING);
  EXPECT(PROBLEM_RAW_ID_INVALID, IDINVALID);
  EXPECT(PROBLEM_RAW_ID_OTHER_BOARD, IDOTHER);
  EXPECT(PROBLEM_RAW_ID_HARDWARE_KEY, IDHARDWARE);
  EXPECT(PROBLEM_RAW_SIGNER_MISMATCH, SIGNERMISMATCH);
  EXPECT(PROBLEM_RAW_ID_HARDWARE_UNPROTECTED, IDHARDWAREUNPROT);
  EXPECT(PROBLEM_RAW_SIGNER_UNPROTECTED, SIGNERUNPROT);
  EXPECT(PROBLEM_RAW_SIGN_FAILED, SIGNFAIL);
  EXPECT("REFUSED: BAD SIGNATURE (HTTP 401)", AUTHBAD);  // any case
  EXPECT("No Identity Record", IDMISSING);
  // HTTP status codes
  EXPECT("HTTP 401", HTTPAUTH);
  EXPECT("HTTP 403", HTTPAUTH);
  EXPECT("HTTP 404", NOTSVC);
  EXPECT("HTTP 429", LIMIT);
  for (int s : {500, 501, 502, 503, 504, 520, 599}) { char b[16]; snprintf(b, sizeof b, "HTTP %d", s); EXPECT(b, DOWN); }
  for (int s : {100, 204, 301, 302, 304, 400, 402, 405, 418, 451, 499, 600, 999}) { char b[16]; snprintf(b, sizeof b, "HTTP %d", s); EXPECT(b, ODDSTATUS); }
  EXPECT("HTTP 502 Bad Gateway", DOWN);
  EXPECT("HTTP 404 Not Found", NOTSVC);
  EXPECT("HTTP  502", DOWN);  // two spaces
  // DNS
  EXPECT(PROBLEM_RAW_NO_ADDRESS, DNS);
  EXPECT("No Address Found For The Host Name", DNS);
  EXPECT("could not resolve host", DNS);
  EXPECT("Failed to resolve api.example.com", DNS);
  // certificate dates and trust (the connection library's texts)
  EXPECT("The certificate validity has expired", CERT);
  EXPECT("The certificate validity starts in the future", CERT);
  EXPECT("X509 - Certificate verification failed: certificate is not yet valid", CERT);
  EXPECT("certificate has expired", CERT);
  EXPECT("CERTIFICATE EXPIRED", CERT);
  EXPECT("X509 - Certificate verification failed, e.g. CRL, CA or signature check failed", CERT);
  EXPECT("The certificate is not correctly signed by the trusted CA", CERT);
  EXPECT("The certificate Common Name (CN) does not match with the expected CN", CERT);
  EXPECT("x509", CERT);
  // timeouts
  EXPECT("handshake timeout", HSTIMEOUT);
  EXPECT("SSL handshake timed out", HSTIMEOUT);
  EXPECT("Handshake Timed Out", HSTIMEOUT);
  EXPECT("SSL - The operation timed out", TIMEOUT);
  EXPECT("timeout", TIMEOUT);
  EXPECT("Read Timeout", TIMEOUT);
  // refused / closed
  EXPECT("connection refused", REFUSED);
  EXPECT("Connect refused", REFUSED);
  EXPECT("SSL - The connection indicated an EOF", CLOSED);
  EXPECT("NET - Connection was reset by peer", CLOSED);
  EXPECT("SSL - The peer notified us that the connection is going to be closed", CLOSED);
  EXPECT("connection closed", CLOSED);
  EXPECT("EOF", CLOSED);
  EXPECT("Connection reset", CLOSED);
  // the firmware's own words from live.cpp
  EXPECT(PROBLEM_RAW_UNEXPECTED, UNREADABLE);
  EXPECT("Unexpected Answer", UNREADABLE);
  EXPECT(PROBLEM_RAW_BUSY, BUSY);
  EXPECT("Busy With Another Request", BUSY);
}

static void testWordsConnectPrefixes() {
  SECTION("problemWords: connectDual's \"v6/v4 connect failed: ...\" prefixes");
  EXPECT("v4 connect failed: ", NOCONN);  // lastError came back empty
  EXPECT("v6 connect failed: ", NOCONN);
  EXPECT("V4 Connect Failed", NOCONN);
  EXPECT("v4 connect failed: NET - Failed to open a connection", NOCONN);
  EXPECT("v6 connect failed: SSL - A fatal alert message was received from our peer", NOCONN);
  EXPECT("v4 connect failed: UNKNOWN ERROR CODE (0050)", NOCONN);
  // the reason after the prefix is what counts
  EXPECT("v4 connect failed: SSL - The operation timed out", TIMEOUT);
  EXPECT("v6 connect failed: SSL - Handshake timeout", HSTIMEOUT);
  EXPECT("v4 connect failed: X509 - Certificate verification failed, e.g. CRL, CA or signature check failed", CERT);
  EXPECT("v6 connect failed: X509 - The certificate validity has expired", CERT);
  EXPECT("v4 connect failed: SSL - The connection indicated an EOF", CLOSED);
  EXPECT("v6 connect failed: NET - Connection was reset by peer", CLOSED);
  EXPECT("v4 connect failed: connection refused", REFUSED);
  EXPECT("v6 connect failed: " PROBLEM_RAW_NO_ADDRESS, DNS);
  // the prefix and its family never reach the sentence, but the detail keeps them
  Words r = words("v6 connect failed: SSL - The operation timed out");
  CHECK(r.detail == "v6 connect failed: SSL - The operation timed out");
}

static void testWordsOrdering() {
  SECTION("problemWords: texts with more than one clue");
  EXPECT("HTTP 401 token", HTTPAUTH);
  EXPECT("HTTP 401 invalid token", HTTPAUTH);
  EXPECT("token invalid, HTTP 403", HTTPAUTH);
  // a known reason beats the status and the word "refused"; an unknown one is just a refused request
  EXPECT("refused: bad signature (HTTP 401)", AUTHBAD);
  EXPECT("  " PROBLEM_RAW_AUTH_REVOKED, AUTHREVOKED);               // leading spaces are not part of the text
  EXPECT("HTTP 401 " PROBLEM_RAW_AUTH_CLOCK_SKEW " (retried)", AUTHCLOCK);
  EXPECT("refused: signature missing (HTTP 401) connection refused", AUTHMISSING);
  EXPECT("HTTP 401 refused: something else", HTTPAUTH);
  EXPECT("refused: rate limited (HTTP 401)", HTTPAUTH);
  EXPECT("no identity record (HTTP 401)", IDMISSING);
  EXPECT("The service needs an access token (HTTP 401)", HTTPAUTH);  // an older service: still only a refused request
  EXPECT("HTTP 503: connection timed out", DOWN);
  EXPECT("HTTP 404 connection refused", NOTSVC);
  EXPECT("HTTP 502 certificate expired", DOWN);
  EXPECT("could not resolve host: timed out", DNS);
  EXPECT(PROBLEM_RAW_NO_ADDRESS " (timeout)", DNS);
  EXPECT("certificate expired, handshake timeout", CERT);
  EXPECT("handshake failed: certificate expired", CERT);
  EXPECT("X509 - Certificate verification failed, e.g. CRL, CA or signature check failed (timed out)", CERT);
  EXPECT("v4 connect failed: SSL - The handshake negotiation failed", NOCONN);  // "handshake" alone is not a timeout
  EXPECT("v4 connect failed: SSL - The operation timed out while waiting for the server hello", TIMEOUT);
  // the status wins over a connection word in the same text only when the status was really received
  EXPECT("HTTP 200", ODDSTATUS);  // a 200 is never reported as an error, but if it is it must not be hidden as "fine"
}

static void testWordsGreedy() {
  SECTION("problemWords: clues inside unrelated words or unrelated texts");
  EXPECT("Geofence list is empty", PASS);          // "eof" inside "Geofence"
  EXPECT("The value is whereof unknown", PASS);    // "eof" inside "whereof"
  EXPECT("Preset 3 is missing", PASS);             // "reset" inside "Preset"
  EXPECT("The box is enclosed", PASS);             // "closed" inside "enclosed"
  EXPECT("The access token has expired", PASS);    // "expired" with no certificate in the text
  EXPECT("The session expired", PASS);
  EXPECT("token", PASS);
  EXPECT("HTTP", PASS);          // no status number
  EXPECT("HTTP abc", PASS);
  EXPECT("http", PASS);
  // texts a status code could be confused with
  EXPECT("Error 404 while loading", PASS);         // no "HTTP " in front of the number
  EXPECT("HTTP/1.1 502", PASS);                    // "HTTP/", not "HTTP "
}

static void testWordsCaseAndStatusForms() {
  SECTION("problemWords: case and status forms");
  EXPECT("http 502", DOWN);
  EXPECT("Http 401", HTTPAUTH);
  EXPECT("HTTP 502abc", DOWN);
  Words r = words("HTTP 502");
  CHECK(r.why.find("502") != std::string::npos);
  r = words("HTTP 401");
  CHECK(r.why.find("HTTP 401") != std::string::npos);
  CHECK(problemHas(r.why.c_str(), "address"));   // tells the owner where to look
  CHECK(!problemHas(r.why.c_str(), "token"));
  r = words("HTTP 403");
  CHECK(r.why.find("HTTP 403") != std::string::npos);
  r = words("HTTP 404");
  CHECK(r.why.find("HTTP 404") != std::string::npos);
  CHECK(problemHas(r.why.c_str(), "address"));
  r = words("HTTP 429");
  CHECK(r.why.find("HTTP 429") != std::string::npos);
  r = words("HTTP 418");
  CHECK(r.why.find("HTTP 418") != std::string::npos);
  r = words("HTTP 99999");
  CHECK(r.tag == ODDSTATUS && r.why.find("99999") != std::string::npos);
  // "HTTP 0" (a status line that could not be read) is the same thing as "no answer"; live.cpp now reports PROBLEM_RAW_NO_ANSWER
  EXPECT("HTTP 0", NOANSWER);
  EXPECT("http 0", NOANSWER);
  Words z = words("HTTP 0");
  CHECK(z.detail == "");  // a bare status: the sentence says it all
}

static void testRawMacros() {
  SECTION("problemWords: the PROBLEM_RAW_* texts live.cpp reports");
  EXPECT(PROBLEM_RAW_NO_ANSWER, NOANSWER);
  EXPECT(PROBLEM_RAW_UNEXPECTED, UNREADABLE);
  EXPECT(PROBLEM_RAW_BUSY, BUSY);
  EXPECT(PROBLEM_RAW_NO_ADDRESS, DNS);
  // each is shown with its own raw text as the detail
  for (const char *raw : {PROBLEM_RAW_NO_ANSWER, PROBLEM_RAW_UNEXPECTED, PROBLEM_RAW_BUSY, PROBLEM_RAW_NO_ADDRESS}) {
    Words r = words(raw);
    CHECK(r.detail == raw);
    CHECK(r.why != raw);
  }
  // "no answer" contains "closed" and "timed out" but is its own sentence, before the plain timeout and closed ones
  Words na = words(PROBLEM_RAW_NO_ANSWER);
  CHECK(na.tag == NOANSWER);
  CHECK(na.why != words("SSL - The operation timed out").why);
  CHECK(na.why != words("SSL - The connection indicated an EOF").why);
  CHECK(problemHas(na.why.c_str(), "hung up") && problemHas(na.why.c_str(), "in time"));  // covers both causes
  // and the neighbouring ones are not taken for it
  EXPECT("SSL - The operation timed out", TIMEOUT);
  EXPECT("NET - Connection was reset by peer", CLOSED);
  EXPECT("no answer", PASS);  // not the firmware's text: shown as it came
  // the macros are the whole text, also with the leading spaces and in other case the classifier tolerates
  EXPECT("  " PROBLEM_RAW_NO_ANSWER, NOANSWER);
  EXPECT("No Answer: The Connection Closed Or Timed Out", NOANSWER);
}

static void testWordsSentences() {
  SECTION("problemWords: what the sentences say");
  // every certificate text gets the one sentence, which names the three causes the core cannot tell apart
  for (const char *raw : {"X509 - Certificate verification failed, e.g. CRL, CA or signature check failed",
                          "The certificate validity has expired", "v4 connect failed: X509 - Certificate verification failed", "x509"}) {
    Words c = words(raw);
    CHECK(c.tag == CERT);
    CHECK(problemHas(c.why.c_str(), "certificate"));
    CHECK(problemHas(c.why.c_str(), "clock"));
    CHECK(problemHas(c.why.c_str(), "authority"));
    CHECK(problemHas(c.why.c_str(), "real server"));
  }
  Words r = words(PROBLEM_RAW_NO_ANSWER);
  CHECK(problemHas(r.why.c_str(), "answer"));
  r = words(PROBLEM_RAW_NO_ADDRESS);
  CHECK(problemHas(r.why.c_str(), "DNS"));
  CHECK(problemHas(r.why.c_str(), "address"));
  r = words(PROBLEM_RAW_ID_MISSING);
  CHECK(r.why == "This board has no identity record. It must be provisioned over USB.");
  r = words(PROBLEM_RAW_ID_INVALID);
  CHECK(problemHas(r.why.c_str(), "USB") && problemHas(r.why.c_str(), "again"));
  r = words(PROBLEM_RAW_ID_OTHER_BOARD);
  CHECK(problemHas(r.why.c_str(), "another board") && problemHas(r.why.c_str(), "USB"));
  r = words(PROBLEM_RAW_ID_HARDWARE_KEY);
  CHECK(problemHas(r.why.c_str(), "the chip has no key") && problemHas(r.why.c_str(), "USB"));
  r = words(PROBLEM_RAW_SIGNER_MISMATCH);
  CHECK(problemHas(r.why.c_str(), "saved key") && problemHas(r.why.c_str(), "keeps signing"));
  r = words(PROBLEM_RAW_ID_HARDWARE_UNPROTECTED);
  CHECK(problemHas(r.why.c_str(), "not fully protected") && problemHas(r.why.c_str(), "USB"));
  r = words(PROBLEM_RAW_SIGNER_UNPROTECTED);
  CHECK(problemHas(r.why.c_str(), "not fully protected") && problemHas(r.why.c_str(), "saved key"));
  r = words(PROBLEM_RAW_AUTH_BAD_SIGNATURE);
  CHECK(problemHas(r.why.c_str(), "The service does not accept this board's key."));
  CHECK(problemHas(r.why.c_str(), "USB"));
  r = words(PROBLEM_RAW_AUTH_REVOKED);
  CHECK(problemHas(r.why.c_str(), "revoked") && problemHas(r.why.c_str(), "owner"));
  r = words(PROBLEM_RAW_AUTH_CLOCK_SKEW);
  CHECK(problemHas(r.why.c_str(), "clock"));
  r = words(PROBLEM_RAW_AUTH_MISSING);
  CHECK(problemHas(r.why.c_str(), "signature"));
  r = words(PROBLEM_RAW_AUTH_MALFORMED);
  CHECK(problemHas(r.why.c_str(), "signature"));
  r = words("HTTP 502");
  CHECK(problemHas(r.why.c_str(), "down"));
  r = words("HTTP 429");
  CHECK(problemHas(r.why.c_str(), "itself") || problemHas(r.why.c_str(), "recover"));
  // sentences start with a capital letter
  for (const char *raw : {"HTTP 502", PROBLEM_RAW_NO_ADDRESS, PROBLEM_RAW_NO_ANSWER, "SSL - The operation timed out", "connection refused",
                          "SSL - The connection indicated an EOF", "v4 connect failed: ", PROBLEM_RAW_UNEXPECTED, PROBLEM_RAW_BUSY,
                          PROBLEM_RAW_ID_MISSING, PROBLEM_RAW_ID_INVALID, PROBLEM_RAW_ID_OTHER_BOARD, PROBLEM_RAW_ID_HARDWARE_KEY,
                          PROBLEM_RAW_SIGN_FAILED, PROBLEM_RAW_AUTH_MISSING, PROBLEM_RAW_AUTH_MALFORMED, PROBLEM_RAW_AUTH_BAD_SIGNATURE,
                          PROBLEM_RAW_AUTH_REVOKED, PROBLEM_RAW_AUTH_CLOCK_SKEW, "X509 - Certificate verification failed"}) {
    Words x = words(raw);
    CHECK(!x.why.empty() && x.why[0] >= 'A' && x.why[0] <= 'Z');
  }
}

static void testWordsDeviceAuth() {
  SECTION("problemWords: device authentication (every reason and every identity problem)");
  struct Case { const char *raw; const char *sentence; };
  static const Case cases[] = {
      {PROBLEM_RAW_ID_MISSING, "This board has no identity record. It must be provisioned over USB."},
      {PROBLEM_RAW_ID_INVALID, "This board's identity record is not valid. It must be provisioned again over USB."},
      {PROBLEM_RAW_ID_OTHER_BOARD, "This board's identity record is for another board. It must be provisioned again over USB."},
      {PROBLEM_RAW_ID_HARDWARE_KEY, "This board's record says the key is burned into the chip but the chip has no key. It must be provisioned again over USB."},
      {PROBLEM_RAW_ID_HARDWARE_UNPROTECTED, "This board's record says the key is burned into the chip but the burned key is not fully protected, so it is not used. It must be provisioned again over USB."},
      {PROBLEM_RAW_SIGNER_MISMATCH, "The chip's key and the saved key give different answers, so the board keeps signing with the saved key. The burn may have used the wrong key."},
      {PROBLEM_RAW_SIGNER_UNPROTECTED, "The key in the chip is not fully protected (was a burn interrupted?), so the board does not use it and keeps signing with the saved key."},
      {PROBLEM_RAW_SIGN_FAILED, "The board could not sign the request. Restart it; if this keeps happening the firmware needs an update."},
      {PROBLEM_RAW_AUTH_MISSING, "The service did not receive a signature on the request. Something between the board and the service may be removing it."},
      {PROBLEM_RAW_AUTH_MALFORMED, "The service could not read the signature on the request. The board and the service may run versions that do not match."},
      {PROBLEM_RAW_AUTH_BAD_SIGNATURE, "The service does not accept this board's key. Either the service is set up for other boards, or this board must be provisioned again over USB."},
      {PROBLEM_RAW_AUTH_REVOKED, "The service has revoked this board. The owner of the service has to allow it again."},
      {PROBLEM_RAW_AUTH_CLOCK_SKEW, "The service says this board's clock is too far off from its own. Check that both clocks are right."},
  };
  for (const Case &c : cases) {
    Words r = words(c.raw);
    CHECK(r.why == c.sentence);
    CHECK(r.why.size() < 159);
    // the technical text stays as the detail, and a sentence is never its own detail
    CHECK(r.detail == c.raw);
    // no sentence mentions a secret, a password or an access token: the board has none to enter
    for (const char *w : {"secret", "password", "token", "master", "hmac", "sha"}) CHECK(!problemHas(r.why.c_str(), w));
    // fed back in, a sentence stays itself (the portal and the connection test may pass one on)
    CHECK(words(r.why.c_str()).why == r.why);
  }
  // every raw text has its own sentence
  for (size_t a = 0; a < sizeof cases / sizeof cases[0]; a++)
    for (size_t b = a + 1; b < sizeof cases / sizeof cases[0]; b++) CHECK(strcmp(cases[a].sentence, cases[b].sentence) != 0);
  // a refusal the board cannot read stays a plain refused request (an older service, or a proxy in front of it)
  Words u = words("HTTP 401");
  CHECK(u.tag == HTTPAUTH && u.detail == "");
  CHECK(!problemHas(u.why.c_str(), "token") && problemHas(u.why.c_str(), "address"));
  // the raw texts are what live.cpp reports and identity.cpp returns: short, no secrets, no HTML
  for (const Case &c : cases) CHECK(strlen(c.raw) < 70 && !strchr(c.raw, '<'));
  // in the problem list they are separate lines, each with its own count
  ProblemLog l;
  auto note = [&](const char *raw, uint32_t unixNow, uint32_t upS) {  // what live.cpp's noteProblem does
    char why[sizeof(ProblemEntry::why)], detail[sizeof(ProblemEntry::detail)];
    problemWords(raw, why, sizeof(why), detail, sizeof(detail));
    l.record(ProblemKind::Flights, why, detail, unixNow, upS);
  };
  const size_t N = sizeof cases / sizeof cases[0];
  for (size_t i = N - 10; i < N; i++) note(cases[i].raw, 1000, 10);  // the list holds ten
  CHECK(l.n == 10);
  note(PROBLEM_RAW_AUTH_BAD_SIGNATURE, 1015, 25);
  CHECK(l.n == 10 && l.e[0].count == 2 && words(PROBLEM_RAW_AUTH_BAD_SIGNATURE).why == l.e[0].why);
}

static void testWordsDetail() {
  SECTION("problemWords: the detail keeps the technical text");
  Words r = words("X509 - Certificate verification failed, e.g. CRL, CA or signature check failed");
  CHECK(r.detail == "X509 - Certificate verification failed, e.g. CRL, CA or signature check failed");
  CHECK(r.why != r.detail);
  r = words("SSL - The connection indicated an EOF");
  CHECK(r.detail == "SSL - The connection indicated an EOF");
  r = words("v4 connect failed: NET - Failed to open a connection");
  CHECK(r.detail == "v4 connect failed: NET - Failed to open a connection");
  r = words(PROBLEM_RAW_NO_ADDRESS);
  CHECK(r.detail == PROBLEM_RAW_NO_ADDRESS);
  r = words("   handshake timeout");  // leading spaces are not part of the text
  CHECK(r.detail == "handshake timeout");
  r = words("HTTP 502");  // the sentence already says it all
  CHECK(r.detail == "");
  r = words("HTTP 401");
  CHECK(r.detail == "");
  r = words("HTTP 502 Bad Gateway");
  CHECK(r.detail == "HTTP 502 Bad Gateway");
  // a text this device does not know has no separate detail: the sentence is the text
  r = words("weird failure");
  CHECK(r.detail == "");
}

static void testWordsUnknown() {
  SECTION("problemWords: null, empty, unknown");
  Words r = words(nullptr);
  CHECK(r.why == "Unknown problem." && r.detail == "");
  r = words("");
  CHECK(r.why == "Unknown problem." && r.detail == "");
  r = words("    ");
  CHECK(r.why == "Unknown problem." && r.detail == "");
  r = words("weird failure");
  CHECK(r.why == "Weird failure" && r.tag == PASS);
  r = words("Weird failure");
  CHECK(r.why == "Weird failure");
  r = words("  weird failure");
  CHECK(r.why == "Weird failure");
  r = words("123 failure");
  CHECK(r.why == "123 failure");
  r = words("\xC3\xA9tat");  // the first byte is above 127: untouched, no crash
  CHECK(r.why == "\xC3\xA9tat");
  r = words("a");
  CHECK(r.why == "A");
  r = words("WIFI GONE");
  CHECK(r.why == "WIFI GONE");  // only the first letter changes
  // an unknown text is shown as it came, so a new kind of failure is never hidden
  r = words("some brand new mbedTLS failure -0x7780");
  CHECK(r.tag == PASS && r.why == "Some brand new mbedTLS failure -0x7780");
}

static void testWordsIdempotent() {
  SECTION("problemWords: a plain sentence fed back in stays itself");
  // The portal's connection test already words these; if one ever reaches problemWords again it must not turn into
  // a different sentence.
  for (const char *raw : {PROBLEM_RAW_ID_MISSING, PROBLEM_RAW_ID_INVALID, PROBLEM_RAW_ID_OTHER_BOARD, PROBLEM_RAW_ID_HARDWARE_KEY,
                          PROBLEM_RAW_SIGN_FAILED, PROBLEM_RAW_AUTH_MISSING, PROBLEM_RAW_AUTH_MALFORMED, PROBLEM_RAW_AUTH_BAD_SIGNATURE,
                          PROBLEM_RAW_AUTH_REVOKED, PROBLEM_RAW_AUTH_CLOCK_SKEW, "HTTP 401", "HTTP 403", "HTTP 404",
                          "HTTP 429", "HTTP 502", "HTTP 418", "HTTP 0", PROBLEM_RAW_NO_ADDRESS, PROBLEM_RAW_NO_ANSWER,
                          "The certificate validity has expired", "X509 - Certificate verification failed", "handshake timeout",
                          "SSL - The operation timed out", "connection refused", "SSL - The connection indicated an EOF",
                          "v4 connect failed: ", PROBLEM_RAW_UNEXPECTED, PROBLEM_RAW_BUSY}) {
    Words a = words(raw);
    Words b = words(a.why.c_str());
    checks++;
    if (a.why != b.why) { fails++; printf("    FAIL: \"%s\" -> \"%s\" -> \"%s\"\n", raw, a.why.c_str(), b.why.c_str()); }
  }
  // the sentences themselves, as the portal's connection test and the portal's own identity note word them
  EXPECT("This board has no identity record. It must be provisioned over USB.", IDMISSING);
  EXPECT("This board's identity record is not valid. It must be provisioned again over USB.", IDINVALID);
  EXPECT("This board's identity record is for another board. It must be provisioned again over USB.", IDOTHER);
  EXPECT("The service does not accept this board's key. Either the service is set up for other boards, or this board must be "
         "provisioned again over USB.", AUTHBAD);
}

static void testWordsBounds() {
  SECTION("problemWords: long input, small buffers");
  // 1000 characters, no clue in them: cut to fit, NUL-terminated, nothing written past the end
  std::string longText(1000, 'a');
  Words r = words(longText.c_str());
  CHECK(r.why.size() == 159);
  CHECK(r.why[0] == 'A' && r.why.substr(1) == longText.substr(1, 158));
  CHECK(r.detail == "");
  // a long text with a clue: the sentence is complete, the detail is cut
  std::string longCert = "x509 " + std::string(1000, 'b');
  r = words(longCert.c_str());
  CHECK(r.tag == CERT);
  CHECK(r.detail.size() == 79);
  CHECK(r.detail == longCert.substr(0, 79));
  std::string longPrefix = "v4 connect failed: " + std::string(500, 'c');
  r = words(longPrefix.c_str());
  CHECK(r.tag == NOCONN && r.detail.size() == 79);
  // exact boundaries of the detail (cap 80): 79 fits, 80 and 81 are cut to 79
  for (size_t len : {78u, 79u, 80u, 81u}) {
    std::string t = "handshake timeout" + std::string(len - 17, 'd');
    r = words(t.c_str());
    CHECK(r.tag == HSTIMEOUT);
    CHECK(r.detail == t.substr(0, 79));
  }
  // exact boundaries of the unknown text (cap 160): 159 fits, 160 and 161 are cut to 159
  for (size_t len : {158u, 159u, 160u, 161u}) {
    std::string t(len, 'e');
    r = words(t.c_str());
    CHECK(r.why.size() == (len < 159 ? len : 159));
  }
  // tiny caps
  for (size_t cap : {1u, 2u, 5u, 9u}) {
    for (const char *raw : {"HTTP 502", "x509 certificate", "weird failure", "", PROBLEM_RAW_NO_ADDRESS, "HTTP 401"}) {
      Guard<16> w, d;
      problemWords(raw, w.buf, cap, d.buf, cap);
      checks += 3;
      if (!w.guardsOk() || !d.guardsOk()) { fails++; printf("    FAIL: cap %zu overran a buffer for \"%s\"\n", cap, raw); }
      if (!memchr(w.buf, 0, cap) || !memchr(d.buf, 0, cap)) { fails++; printf("    FAIL: cap %zu not NUL-terminated for \"%s\"\n", cap, raw); }
      if (w.buf[cap] != 'Z' || d.buf[cap] != 'Z') { fails++; printf("    FAIL: cap %zu wrote past the cap for \"%s\"\n", cap, raw); }
    }
  }
  // a cap of 0 means no room: that buffer is left alone (and the other one still works); null buffers are fine too
  for (const char *raw : {"HTTP 502", "x509 certificate", "weird failure", "", PROBLEM_RAW_NO_ADDRESS, "HTTP 401",
                          PROBLEM_RAW_AUTH_BAD_SIGNATURE, PROBLEM_RAW_ID_MISSING, "HTTP 0"}) {
    {
      Guard<16> w, d;
      problemWords(raw, w.buf, 0, d.buf, 0);
      checks++;
      if (!w.guardsOk() || !d.guardsOk() || strspn(w.buf, "Z") != 16 || strspn(d.buf, "Z") != 16) { fails++; printf("    FAIL: caps 0 wrote something for \"%s\"\n", raw); }
    }
    {
      Guard<16> w;
      Guard<80> d;
      problemWords(raw, w.buf, 0, d.buf, sizeof d.buf);  // no room for the sentence, the detail still comes
      Guard<160> w2;
      Guard<16> d2;
      problemWords(raw, w2.buf, sizeof w2.buf, d2.buf, 0);  // and the other way round
      checks++;
      if (!w.guardsOk() || !d.guardsOk() || strspn(w.buf, "Z") != 16 || !memchr(d.buf, 0, sizeof d.buf)) { fails++; printf("    FAIL: whyCap 0 for \"%s\"\n", raw); }
      checks++;
      if (!w2.guardsOk() || !d2.guardsOk() || strspn(d2.buf, "Z") != 16 || !memchr(w2.buf, 0, sizeof w2.buf)) { fails++; printf("    FAIL: detailCap 0 for \"%s\"\n", raw); }
      Words full = words(raw);
      checks++;
      if (strcmp(d.buf, full.detail.c_str()) != 0) { fails++; printf("    FAIL: detail differs when whyCap is 0 for \"%s\"\n", raw); }
      checks++;
      if (strcmp(w2.buf, full.why.c_str()) != 0) { fails++; printf("    FAIL: sentence differs when detailCap is 0 for \"%s\"\n", raw); }
    }
    char tmp[8] = "ZZZZZZZ";
    problemWords(raw, nullptr, 160, tmp, 0);  // null why with a stated cap, nothing else to write to
    problemWords(raw, tmp, 0, nullptr, 80);
    problemWords(raw, nullptr, 0, nullptr, 0);
    problemWords(nullptr, nullptr, 0, nullptr, 0);
    checks++;
    if (strcmp(tmp, "ZZZZZZZ") != 0) { fails++; printf("    FAIL: zero-cap call wrote to a buffer for \"%s\"\n", raw); }
  }
  // the longest sentence still fits in the 160 bytes of ProblemEntry::why (the "ends with a full stop" check in words() would
  // catch a cut); check the longest branches explicitly
  r = words("X509 - Certificate verification failed");
  CHECK(r.why.size() < 159);
  r = words("HTTP 599");
  CHECK(r.why.size() < 159);
  r = words("HTTP 2147483");  // a long status number
  CHECK(r.tag == ODDSTATUS && r.why.size() < 159);
}

// ---- ProblemLog -----------------------------------------------------------------------------------------------------------
// What live.cpp's noteProblem does with a raw text: words first, then the list.
static void recordRaw(ProblemLog &l, ProblemKind kind, const char *raw, uint32_t unixNow, uint32_t upS) {
  char why[sizeof(ProblemEntry::why)], detail[sizeof(ProblemEntry::detail)];
  problemWords(raw, why, sizeof(why), detail, sizeof(detail));
  l.record(kind, why, detail, unixNow, upS);
}

static const ProblemKind FL = ProblemKind::Flights, WE = ProblemKind::Weather, HE = ProblemKind::Headlines, LO = ProblemKind::Logo,
                         CL = ProblemKind::Clock, UP = ProblemKind::Update;

// the list holds exactly these `why` texts, newest first
static bool order(const ProblemLog &l, std::initializer_list<const char *> whys) {
  if (l.n != (int)whys.size()) return false;
  int i = 0;
  for (const char *w : whys) if (strcmp(l.e[i++].why, w) != 0) return false;
  return true;
}

static void testLogBasics() {
  SECTION("ProblemLog: basics");
  ProblemLog l;
  CHECK(l.n == 0);
  l.record(FL, "a", "detail a", 1000, 10);
  CHECK(l.n == 1);
  CHECK(l.e[0].kind == FL && l.e[0].count == 1);
  CHECK(l.e[0].firstUnix == 1000 && l.e[0].lastUnix == 1000);
  CHECK(l.e[0].firstUpS == 10 && l.e[0].lastUpS == 10);
  CHECK(strcmp(l.e[0].why, "a") == 0 && strcmp(l.e[0].detail, "detail a") == 0);
  l.record(WE, "b", "", 1100, 110);
  l.record(HE, "c", nullptr, 1200, 210);
  CHECK(order(l, {"c", "b", "a"}));  // newest first
  CHECK(l.e[0].kind == HE && l.e[1].kind == WE && l.e[2].kind == FL);
  CHECK(strcmp(l.e[0].detail, "") == 0 && strcmp(l.e[1].detail, "") == 0);  // a null and an empty detail are both ""
  CHECK(l.e[0].firstUnix == 1200 && l.e[1].firstUnix == 1100 && l.e[2].firstUnix == 1000);
  CHECK(l.e[2].count == 1 && l.e[1].count == 1 && l.e[0].count == 1);
  for (int k = 0; k < PROBLEM_KINDS; k++) {  // every kind can be recorded and read back
    ProblemLog m;
    m.record((ProblemKind)k, "x", "", 1, 1);
    CHECK((int)m.e[0].kind == k);
  }
}

static void testLogRepeat() {
  SECTION("ProblemLog: the same problem again");
  ProblemLog l;
  l.record(FL, "a", "first detail", 1000, 10);
  l.record(WE, "b", "", 1100, 110);
  l.record(HE, "c", "", 1200, 210);
  l.record(FL, "a", "second detail", 5000, 400);  // the oldest again: back to the top
  CHECK(order(l, {"a", "c", "b"}));
  CHECK(l.e[0].count == 2);
  CHECK(l.e[0].firstUnix == 1000 && l.e[0].lastUnix == 5000);
  CHECK(l.e[0].firstUpS == 10 && l.e[0].lastUpS == 400);
  CHECK(strcmp(l.e[0].detail, "second detail") == 0);
  CHECK(l.e[0].kind == FL);
  CHECK(l.e[1].count == 1 && l.e[2].count == 1);
  // the middle one
  l.record(HE, "c", "", 6000, 500);
  CHECK(order(l, {"c", "a", "b"}));
  CHECK(l.e[0].count == 2 && l.e[0].firstUnix == 1200 && l.e[0].lastUnix == 6000);
  CHECK(l.e[1].count == 2 && l.e[2].count == 1);
  // the top one again: stays on top, counts up
  l.record(HE, "c", "", 7000, 600);
  CHECK(order(l, {"c", "a", "b"}));
  CHECK(l.e[0].count == 3 && l.e[0].lastUnix == 7000 && l.e[0].firstUnix == 1200);
  CHECK(l.e[1].count == 2 && l.e[2].count == 1 && l.e[2].lastUnix == 1100);
  // the oldest of three, with a null detail: the old detail stays rather than becoming garbage
  l.record(WE, "b", nullptr, 8000, 700);
  CHECK(order(l, {"b", "c", "a"}));
  CHECK(l.e[0].count == 2 && strlen(l.e[0].detail) < 80);
}

static void testLogIdentity() {
  SECTION("ProblemLog: what makes two problems the same");
  ProblemLog l;
  l.record(FL, "same words", "", 1, 1);
  l.record(WE, "same words", "", 2, 2);  // same words, other kind: a different problem
  CHECK(l.n == 2);
  CHECK(l.e[0].kind == WE && l.e[1].kind == FL && l.e[0].count == 1 && l.e[1].count == 1);
  l.record(FL, "same words", "", 3, 3);
  CHECK(l.n == 2 && l.e[0].kind == FL && l.e[0].count == 2 && l.e[1].kind == WE && l.e[1].count == 1);
  l.record(WE, "same words", "", 4, 4);
  CHECK(l.n == 2 && l.e[0].kind == WE && l.e[0].count == 2 && l.e[1].kind == FL && l.e[1].count == 2);
  l.record(FL, "other words", "", 5, 5);  // same kind, other words
  CHECK(l.n == 3 && l.e[0].count == 1);
  // one text being the start of another is not the same text
  ProblemLog m;
  m.record(FL, "abc", "", 1, 1);
  m.record(FL, "abcd", "", 2, 2);
  m.record(FL, "ab", "", 3, 3);
  m.record(FL, "", "", 4, 4);
  CHECK(m.n == 4);
  m.record(FL, "abc", "", 5, 5);
  CHECK(m.n == 4 && m.e[0].count == 2 && strcmp(m.e[0].why, "abc") == 0);
  // case counts: these are generated texts, so a different case is a different text
  ProblemLog c;
  c.record(FL, "Abc", "", 1, 1);
  c.record(FL, "abc", "", 2, 2);
  CHECK(c.n == 2);
  // all kinds, same words: one entry per kind
  ProblemLog k;
  for (int i = 0; i < PROBLEM_KINDS; i++) k.record((ProblemKind)i, "x", "", i, i);
  CHECK(k.n == PROBLEM_KINDS);
  for (int i = 0; i < PROBLEM_KINDS; i++) CHECK((int)k.e[i].kind == PROBLEM_KINDS - 1 - i);
}

static void testLogCapacity() {
  SECTION("ProblemLog: ten distinct problems, the oldest drops");
  ProblemLog l;
  char name[16];
  for (int i = 0; i < 10; i++) { snprintf(name, sizeof name, "p%d", i); l.record(FL, name, "", 100 + i, i); }
  CHECK(l.n == 10);
  for (int i = 0; i < 10; i++) { snprintf(name, sizeof name, "p%d", 9 - i); CHECK(strcmp(l.e[i].why, name) == 0); }
  l.record(FL, "p10", "", 200, 50);  // the 11th distinct one: p0 falls off
  CHECK(l.n == 10);
  CHECK(strcmp(l.e[0].why, "p10") == 0 && strcmp(l.e[1].why, "p9") == 0 && strcmp(l.e[9].why, "p1") == 0);
  bool hasP0 = false;
  for (int i = 0; i < l.n; i++) if (strcmp(l.e[i].why, "p0") == 0) hasP0 = true;
  CHECK(!hasP0);
  CHECK(l.e[0].count == 1 && l.e[0].firstUnix == 200 && l.e[0].firstUpS == 50);
  CHECK(l.e[9].firstUnix == 101 && l.e[9].count == 1);  // p1's own numbers survived the shuffle
  // the evicted problem is new when it comes back
  l.record(FL, "p0", "", 300, 60);
  CHECK(l.n == 10 && strcmp(l.e[0].why, "p0") == 0 && l.e[0].count == 1 && l.e[0].firstUnix == 300);
  CHECK(strcmp(l.e[9].why, "p2") == 0);
  // 25 distinct: always the last ten, newest first, every field in place
  ProblemLog m;
  for (int i = 0; i < 25; i++) { snprintf(name, sizeof name, "q%d", i); m.record((ProblemKind)(i % PROBLEM_KINDS), name, name, 1000 + i, 20 + i); }
  CHECK(m.n == 10);
  for (int i = 0; i < 10; i++) {
    int id = 24 - i;
    snprintf(name, sizeof name, "q%d", id);
    CHECK(strcmp(m.e[i].why, name) == 0 && strcmp(m.e[i].detail, name) == 0);
    CHECK((int)m.e[i].kind == id % PROBLEM_KINDS);
    CHECK(m.e[i].firstUnix == (uint32_t)(1000 + id) && m.e[i].lastUnix == (uint32_t)(1000 + id));
    CHECK(m.e[i].firstUpS == (uint32_t)(20 + id) && m.e[i].count == 1);
  }
  // exactly ten is not eleven: no problem is lost at the limit
  ProblemLog t;
  for (int i = 0; i < 10; i++) { snprintf(name, sizeof name, "t%d", i); t.record(FL, name, "", i, i); }
  CHECK(t.n == 10 && strcmp(t.e[9].why, "t0") == 0);
}

static void testLogFlood() {
  SECTION("ProblemLog: a failure repeating every 15 s keeps the older problems");
  ProblemLog l;
  char name[16];
  l.record(CL, "oldest problem", "technical", 100, 1);
  for (int i = 1; i < 9; i++) { snprintf(name, sizeof name, "other %d", i); l.record(WE, name, "", 100 + i, 1 + i); }
  l.record(FL, "flood", "", 200, 20);
  CHECK(l.n == 10 && strcmp(l.e[9].why, "oldest problem") == 0);
  for (uint32_t i = 1; i <= 5760; i++) l.record(FL, "flood", "", 200 + i * 15, 20 + i * 15);  // a whole day
  CHECK(l.n == 10);
  CHECK(strcmp(l.e[0].why, "flood") == 0 && l.e[0].count == 5761);
  CHECK(strcmp(l.e[9].why, "oldest problem") == 0 && l.e[9].count == 1 && l.e[9].firstUnix == 100);
  CHECK(l.e[0].firstUnix == 200 && l.e[0].lastUnix == 200 + 5760 * 15);
  // two repeating problems taking turns (the second is one the list already had)
  for (uint32_t i = 0; i < 1000; i++) {
    l.record(FL, "flood", "", 90000 + i, 1);
    l.record(WE, "other 3", "", 90000 + i, 1);
  }
  CHECK(l.n == 10);
  CHECK(strcmp(l.e[0].why, "other 3") == 0 && strcmp(l.e[1].why, "flood") == 0 && l.e[0].count == 1001);
  CHECK(strcmp(l.e[9].why, "oldest problem") == 0);
  // through problemWords then record, the way live.cpp does
  ProblemLog r;
  recordRaw(r, UP, "HTTP 503", 10, 10);
  for (int i = 0; i < 500; i++) recordRaw(r, FL, "v4 connect failed: SSL - The operation timed out", 20 + i, 20 + i);
  CHECK(r.n == 2 && r.e[0].count == 500 && r.e[1].count == 1 && r.e[1].kind == UP);
}

static void testLogCounter() {
  SECTION("ProblemLog: the counter stops at 65535");
  ProblemLog l;
  l.record(FL, "again", "", 1, 1);
  l.record(WE, "other", "", 2, 2);
  for (uint32_t i = 0; i < 70000; i++) l.record(FL, "again", "", 10 + i, 10 + i);
  CHECK(l.n == 2);
  CHECK(l.e[0].count == 65535);
  CHECK(l.e[0].lastUnix == 10 + 69999 && l.e[0].lastUpS == 10 + 69999);  // still updated once the count is full
  CHECK(l.e[0].firstUnix == 1 && l.e[0].firstUpS == 1);
  CHECK(l.e[1].count == 1);
  // one more after it stopped
  l.record(FL, "again", "", 999999, 999999);
  CHECK(l.e[0].count == 65535 && l.e[0].lastUnix == 999999);
  // and the step into the maximum: 65534 -> 65535
  ProblemLog m;
  m.record(FL, "x", "", 1, 1);
  for (int i = 0; i < 65533; i++) m.record(FL, "x", "", 2, 2);
  CHECK(m.e[0].count == 65534);
  m.record(FL, "x", "", 3, 3);
  CHECK(m.e[0].count == 65535);
  m.record(FL, "x", "", 4, 4);
  CHECK(m.e[0].count == 65535);
}

static void testLogClockNotSet() {
  SECTION("ProblemLog: unix time 0 = clock not set");
  ProblemLog l;
  l.record(FL, "a", "", 0, 5);
  CHECK(l.e[0].firstUnix == 0 && l.e[0].lastUnix == 0 && l.e[0].firstUpS == 5 && l.e[0].lastUpS == 5);
  l.record(FL, "a", "", 0, 20);
  CHECK(l.n == 1 && l.e[0].count == 2 && l.e[0].lastUnix == 0 && l.e[0].lastUpS == 20 && l.e[0].firstUpS == 5);
  l.record(FL, "a", "", 1790000000u, 30);  // the clock got set in between: the first time stays "unknown"
  CHECK(l.e[0].firstUnix == 0 && l.e[0].lastUnix == 1790000000u && l.e[0].count == 3);
  l.record(WE, "b", "", 0, 40);
  CHECK(l.e[0].firstUnix == 0 && l.e[0].lastUnix == 0 && l.e[1].lastUnix == 1790000000u);  // other entries keep their own times
  // the largest values
  l.record(HE, "c", "", 0xFFFFFFFFu, 0xFFFFFFFFu);
  CHECK(l.e[0].firstUnix == 0xFFFFFFFFu && l.e[0].lastUpS == 0xFFFFFFFFu);
}

static void testLogTruncation() {
  SECTION("ProblemLog: long texts are cut safely");
  struct Rig { unsigned char pre[64]; ProblemLog log; unsigned char post[64]; Rig() { memset(pre, 0xA5, 64); memset(post, 0xA5, 64); } };
  Rig rig;
  ProblemLog &l = rig.log;
  l.record(FL, "short 1", "d1", 1, 1);
  l.record(FL, "short 2", "d2", 2, 2);
  std::string bigWhy(500, 'w'), bigDetail(500, 'd');
  l.record(WE, bigWhy.c_str(), bigDetail.c_str(), 3, 3);
  CHECK(l.n == 3 && strlen(l.e[0].why) == 159 && strlen(l.e[0].detail) == 79);
  CHECK(bigWhy.compare(0, 159, l.e[0].why) == 0 && bigDetail.compare(0, 79, l.e[0].detail) == 0);
  CHECK(l.e[0].kind == WE && l.e[0].firstUnix == 3 && l.e[0].count == 1);
  // the neighbours are untouched
  CHECK(strcmp(l.e[1].why, "short 2") == 0 && strcmp(l.e[1].detail, "d2") == 0 && l.e[1].kind == FL && l.e[1].count == 1);
  CHECK(strcmp(l.e[2].why, "short 1") == 0 && strcmp(l.e[2].detail, "d1") == 0 && l.e[2].firstUnix == 1);
  // exact boundaries: why 159 fits, 160/161 are cut; detail 79 fits, 80/81 are cut
  for (size_t len : {158u, 159u, 160u, 161u, 1000u}) {
    ProblemLog m;
    m.record(FL, "before", "x", 1, 1);
    std::string w(len, 'u'), d(len, 'v');
    m.record(FL, w.c_str(), d.c_str(), 2, 2);
    CHECK(m.e[0].why[sizeof m.e[0].why - 1] == 0 && m.e[0].detail[sizeof m.e[0].detail - 1] == 0);
    CHECK(strlen(m.e[0].why) == (len < 159 ? len : 159));
    CHECK(strlen(m.e[0].detail) == (len < 79 ? len : 79));
    CHECK(strcmp(m.e[1].why, "before") == 0 && strcmp(m.e[1].detail, "x") == 0);
  }
  // a full list with a huge text going in at the end of the array
  for (int i = 0; i < 12; i++) { char n[16]; snprintf(n, sizeof n, "f%d", i); l.record(CL, n, "", i, i); }
  CHECK(l.n == 10);
  l.record(LO, bigWhy.c_str(), bigDetail.c_str(), 99, 99);
  CHECK(l.n == 10 && strlen(l.e[0].why) == 159 && strcmp(l.e[1].why, "f11") == 0 && strcmp(l.e[9].why, "f3") == 0);
  // repeating the long text refreshes the detail, still cut
  std::string other(300, 'z');
  l.record(LO, bigWhy.c_str(), other.c_str(), 100, 100);
  CHECK(l.n == 10 && strlen(l.e[0].detail) == 79 && l.e[0].detail[0] == 'z' && l.e[0].count == 2);
  bool guardsOk = true;
  for (unsigned char b : rig.pre) if (b != 0xA5) guardsOk = false;
  for (unsigned char b : rig.post) if (b != 0xA5) guardsOk = false;
  CHECK(guardsOk);
  // a missing text is not a crash
  ProblemLog n;
  n.record(FL, nullptr, nullptr, 1, 1);
  CHECK(n.n == 1 && strcmp(n.e[0].why, "") == 0 && strcmp(n.e[0].detail, "") == 0);
  n.record(FL, nullptr, nullptr, 2, 2);
  CHECK(n.n == 1 && n.e[0].count == 2);
}

static void testLogLongRepeat() {
  SECTION("ProblemLog: a text too long to store is still recognised when it repeats");
  // problemWords gives at most 159 characters, but record() takes any text; a text that was cut when stored must still
  // match itself, or every repeat would add a line and push the older problems out
  ProblemLog l;
  l.record(CL, "older problem", "", 1, 1);
  std::string big(400, 'q');
  for (int i = 0; i < 50; i++) l.record(FL, big.c_str(), "", 10 + i, 10 + i);
  CHECK(l.n == 2);
  CHECK(l.e[0].count == 50 && l.e[0].firstUnix == 10 && l.e[0].lastUnix == 59);
  CHECK(strcmp(l.e[1].why, "older problem") == 0);
  std::string exact(159, 'q'), over(160, 'q');
  ProblemLog m;
  m.record(FL, exact.c_str(), "", 1, 1);
  m.record(FL, exact.c_str(), "", 2, 2);
  CHECK(m.n == 1 && m.e[0].count == 2);
  ProblemLog o;
  o.record(FL, over.c_str(), "", 1, 1);
  o.record(FL, over.c_str(), "", 2, 2);
  CHECK(o.n == 1 && o.e[0].count == 2);
}

static void testLogRecordRaw() {
  SECTION("ProblemLog: problemWords, then record (the way live.cpp does it)");
  ProblemLog l;
  recordRaw(l, FL, "HTTP 502", 100, 10);
  recordRaw(l, FL, "HTTP 502", 200, 20);
  CHECK(l.n == 1 && l.e[0].count == 2 && l.e[0].kind == FL);
  CHECK(classify(l.e[0].why) == DOWN && strcmp(l.e[0].detail, "") == 0);
  recordRaw(l, FL, "HTTP 503", 300, 30);  // another status: other words
  CHECK(l.n == 2 && classify(l.e[0].why) == DOWN && l.e[0].count == 1);
  recordRaw(l, WE, "HTTP 503", 300, 30);  // and the same words for another kind
  CHECK(l.n == 3);
  // dual stack: v6 and v4 report the same reason, so they are one problem; the technical text is the newest
  ProblemLog d;
  recordRaw(d, FL, "v6 connect failed: SSL - The operation timed out", 100, 10);
  recordRaw(d, FL, "v4 connect failed: SSL - The operation timed out", 200, 20);
  CHECK(d.n == 1 && d.e[0].count == 2);
  CHECK(strcmp(d.e[0].detail, "v4 connect failed: SSL - The operation timed out") == 0);
  CHECK(d.e[0].firstUnix == 100 && d.e[0].lastUnix == 200);
  // a different reason is another line, and the technical text of each is kept
  recordRaw(d, FL, "v4 connect failed: X509 - Certificate verification failed, e.g. CRL, CA or signature check failed", 300, 30);
  CHECK(d.n == 2 && classify(d.e[0].why) == CERT && strncmp(d.e[0].detail, "v4 connect failed: X509", 23) == 0);
  CHECK(classify(d.e[1].why) == TIMEOUT);
  // null and empty texts are recorded as an unknown problem, not dropped
  ProblemLog n;
  recordRaw(n, CL, nullptr, 1, 1);
  recordRaw(n, CL, "", 2, 2);
  CHECK(n.n == 1 && n.e[0].count == 2 && strcmp(n.e[0].why, "Unknown problem.") == 0);
  // a very long raw text goes through whole classification and storage without trouble, and repeats as one problem
  std::string big = "mystery " + std::string(1000, 'm');
  ProblemLog b;
  recordRaw(b, LO, big.c_str(), 1, 1);
  recordRaw(b, LO, big.c_str(), 2, 2);
  CHECK(b.n == 1 && b.e[0].count == 2 && strlen(b.e[0].why) == 159);
  std::string bigCert = "x509 " + std::string(1000, 'k');
  recordRaw(b, LO, bigCert.c_str(), 3, 3);
  recordRaw(b, LO, bigCert.c_str(), 4, 4);
  CHECK(b.n == 2 && b.e[0].count == 2 && strlen(b.e[0].detail) == 79);
}

// ---- ProblemWatch and the one-hour rule -------------------------------------------------------------------------------------
// The sketch's decision with everything else favourable (live data, a static picture, no night blanking): what is left is the
// one-hour rule and the 5 s phase.
static bool wanted(const ProblemWatch &w, uint32_t nowMs) { return problemMessageWanted(true, w.longFailure(nowMs), nowMs, true, false); }

// The kind fails every `step` ms from t0 for `dur` ms (the last failure is at t0 + dur when dur is a multiple of step).
static void failEvery(ProblemWatch &w, ProblemKind k, uint32_t t0, uint64_t dur, uint32_t step = 15000) {
  for (uint64_t d = 0; d <= dur; d += step) w.failed(k, (uint32_t)(t0 + d));
}

static void scenarioHour(uint32_t T0) {
  // under an hour: no message; at exactly the hour: message
  {
    ProblemWatch w;
    failEvery(w, WE, T0, HOUR - 15000);  // last failure 15 s before the hour is up
    CHECK(!w.kindIsLong(WE, T0 + HOUR - 1));
    CHECK(!w.longFailure(T0 + HOUR - 1));
    CHECK(w.failingForMs(WE, T0 + HOUR - 1) == HOUR - 1);
    CHECK(w.kindIsLong(WE, T0 + HOUR));
    CHECK(w.longFailure(T0 + HOUR));
    CHECK(w.failingForMs(WE, T0 + HOUR) == HOUR);
    CHECK(w.kindIsLong(WE, T0 + HOUR + 1));
  }
  {
    ProblemWatch w;
    failEvery(w, WE, T0, HOUR);  // a failure exactly when the hour is up
    CHECK(w.kindIsLong(WE, T0 + HOUR));
    ProblemWatch v;
    failEvery(v, WE, T0, HOUR - 15000);
    v.failed(WE, T0 + HOUR - 1);  // a failure 1 ms before the hour is up
    CHECK(!v.kindIsLong(WE, T0 + HOUR - 1));
    CHECK(v.kindIsLong(WE, T0 + HOUR));
  }
  {
    ProblemWatch w;
    CHECK(!w.longFailure(T0));
    w.failed(FL, T0);
    CHECK(!w.longFailure(T0));  // one failure is not a long failure
    CHECK(w.failingForMs(FL, T0) == 0 && w.failingForMs(FL, T0 + 1234) == 1234);
    CHECK(w.failingForMs(WE, T0 + 1234) == 0);  // other kinds are not failing
  }
}

static void scenarioSuccessRestarts(uint32_t T0) {
  {
    ProblemWatch w;
    failEvery(w, FL, T0, 30 * MIN);
    w.ok(FL);  // a success after half an hour
    CHECK(!w.longFailure(T0 + 30 * MIN));
    CHECK(w.failingForMs(FL, T0 + 30 * MIN) == 0);
    uint32_t t1 = T0 + 31 * MIN;
    failEvery(w, FL, t1, HOUR - 15000);
    CHECK(!w.kindIsLong(FL, t1 + HOUR - 1));  // an hour since the first failure, but not since the restart
    CHECK(w.kindIsLong(FL, t1 + HOUR));
    CHECK(w.failingForMs(FL, t1 + HOUR) == HOUR);
  }
  {
    ProblemWatch w;  // a success one minute before the hour is up
    failEvery(w, FL, T0, 59 * MIN);
    w.ok(FL);
    failEvery(w, FL, T0 + 60 * MIN, 30 * MIN);
    CHECK(!w.longFailure(T0 + 90 * MIN));
    CHECK(!w.longFailure(T0 + 119 * MIN));
    failEvery(w, FL, T0 + 90 * MIN + 15000, 30 * MIN - 15000);
    CHECK(w.longFailure(T0 + 120 * MIN));
  }
  {
    ProblemWatch w;  // a router restart: down for 20 minutes, then fine
    failEvery(w, FL, T0, 20 * MIN);
    CHECK(!w.longFailure(T0 + 20 * MIN));
    w.ok(FL);
    CHECK(!w.longFailure(T0 + 3 * HOUR));
  }
  {
    ProblemWatch w;  // ok() with nothing failing changes nothing
    w.ok(FL);
    w.ok(WE);
    CHECK(!w.longFailure(T0 + 5 * HOUR));
    failEvery(w, FL, T0, HOUR);
    CHECK(w.longFailure(T0 + HOUR));
    w.ok(FL);
    CHECK(!w.longFailure(T0 + HOUR));  // the problem cleared: no message
    CHECK(!w.kindIsLong(FL, T0 + HOUR + 1));
    w.failed(FL, T0 + HOUR + 15000);  // the very next failure starts a new run, not the old hour
    CHECK(!w.longFailure(T0 + HOUR + 15000));
  }
}

static void scenarioStale(uint32_t T0) {
  {
    ProblemWatch w;  // still failing now: the last failure is recent
    failEvery(w, LO, T0, HOUR + 5 * MIN);
    uint32_t last = T0 + HOUR + 5 * MIN;
    CHECK(w.kindIsLong(LO, last));
    CHECK(w.kindIsLong(LO, last + PROBLEM_FRESH_MS - 1));
    CHECK(!w.kindIsLong(LO, last + PROBLEM_FRESH_MS + 1));  // nothing failed for more than 10 minutes: no longer "now"
    CHECK(!w.kindIsLong(LO, last + 2 * HOUR));
    CHECK(!w.longFailure(last + 2 * HOUR));
  }
  {
    ProblemWatch w;  // one logo failed an hour ago and was never asked for again
    w.failed(LO, T0);
    w.failed(LO, T0 + 1000);
    CHECK(!w.kindIsLong(LO, T0 + HOUR - 1));
    CHECK(!w.kindIsLong(LO, T0 + HOUR + 1000));
    CHECK(!w.kindIsLong(LO, T0 + 2 * HOUR));
    CHECK(!w.longFailure(T0 + 24 * HOUR));
    uint32_t now = T0 + 2 * HOUR;
    CHECK(!wanted(w, now - now % MIN));  // not even at the start of a minute
  }
  {
    ProblemWatch w;  // an old failure that was never followed by success, other kinds fresh: only the fresh ones count
    w.failed(LO, T0);
    failEvery(w, FL, T0, 3 * HOUR);
    uint32_t now = T0 + 3 * HOUR;
    CHECK(w.kindIsLong(FL, now));
    CHECK(!w.kindIsLong(LO, now));
  }
}

// A gap longer than PROBLEM_FRESH_MS with no failure recorded means nothing was tried (Wi-Fi down, never retried): the failure
// was not continuous, so the hour starts again. A gap of exactly PROBLEM_FRESH_MS is still continuous (like kindIsLong's "within").
static void scenarioGap(uint32_t T0) {
  const uint32_t F = PROBLEM_FRESH_MS;
  {
    ProblemWatch w;  // the stale logo: fails, is not asked for again, fails again two hours later, no success in between
    w.failed(LO, T0);
    w.failed(LO, T0 + 2 * HOUR);
    CHECK(!w.kindIsLong(LO, T0 + 2 * HOUR));
    CHECK(!w.longFailure(T0 + 2 * HOUR));
    CHECK(w.failingForMs(LO, T0 + 2 * HOUR) == 0);        // the run restarted with the second failure
    CHECK(w.failingForMs(LO, T0 + 2 * HOUR + 1234) == 1234);
    CHECK(!wanted(w, (T0 + 2 * HOUR) - (T0 + 2 * HOUR) % MIN));
    // from there it takes a full hour of failures again, not less
    failEvery(w, LO, T0 + 2 * HOUR, HOUR - 15000);
    CHECK(!w.kindIsLong(LO, T0 + 3 * HOUR - 1));
    CHECK(w.kindIsLong(LO, T0 + 3 * HOUR));
  }
  {
    ProblemWatch w;  // flights failed for 59 minutes, then 3 hours with no failure recorded, then again
    failEvery(w, FL, T0, 59 * MIN);
    CHECK(!w.longFailure(T0 + 59 * MIN));
    uint32_t again = T0 + 59 * MIN + 3 * HOUR;
    CHECK(!w.longFailure(again - 1));  // during the gap the old run is stale
    w.failed(FL, again);
    CHECK(!w.longFailure(again));
    CHECK(w.failingForMs(FL, again) == 0);
    CHECK(w.failingForMs(FL, again + 5000) == 5000);
    failEvery(w, FL, again, HOUR - 15000);
    CHECK(!w.kindIsLong(FL, again + HOUR - 1));
    CHECK(w.kindIsLong(FL, again + HOUR));
  }
  {
    ProblemWatch w;  // a run that was already long, then a gap: the message ended with the gap and does not come back at once
    failEvery(w, FL, T0, 90 * MIN);
    CHECK(w.longFailure(T0 + 90 * MIN));
    CHECK(!w.longFailure(T0 + 90 * MIN + F + 1));
    w.failed(FL, T0 + 90 * MIN + 3 * HOUR);
    CHECK(!w.longFailure(T0 + 90 * MIN + 3 * HOUR));
    CHECK(w.failingForMs(FL, T0 + 90 * MIN + 3 * HOUR) == 0);
  }
  {
    // failures every 15 s for an hour with no gap: still triggers at exactly the hour
    ProblemWatch w;
    failEvery(w, FL, T0, HOUR);
    ProblemWatch v;
    failEvery(v, FL, T0, HOUR - 15000);
    CHECK(!v.kindIsLong(FL, T0 + HOUR - 1));
    CHECK(v.kindIsLong(FL, T0 + HOUR));
    CHECK(w.kindIsLong(FL, T0 + HOUR));
    CHECK(w.failingForMs(FL, T0 + HOUR) == HOUR);
  }
  {
    // the boundary: a gap of exactly PROBLEM_FRESH_MS keeps the run, one ms more restarts it
    ProblemWatch keep, restart;
    keep.failed(FL, T0);
    keep.failed(FL, T0 + F);
    CHECK(keep.failingForMs(FL, T0 + F + 5) == F + 5);
    restart.failed(FL, T0);
    restart.failed(FL, T0 + F + 1);
    CHECK(restart.failingForMs(FL, T0 + F + 1) == 0);
    CHECK(restart.failingForMs(FL, T0 + F + 6) == 5);
    // the same through a whole hour: failures spaced exactly F apart are one run, F + 1 apart are many short ones
    ProblemWatch a, b;
    for (uint64_t d = 0; d <= HOUR; d += F) a.failed(FL, (uint32_t)(T0 + d));
    CHECK(a.kindIsLong(FL, T0 + HOUR));
    CHECK(a.failingForMs(FL, T0 + HOUR) == HOUR);
    uint32_t t = T0;
    for (int i = 0; i < 7; i++) { b.failed(FL, t); t += F + 1; }
    CHECK(!b.kindIsLong(FL, t) && !b.longFailure(t));
    CHECK(b.failingForMs(FL, t - (F + 1) + 100) == 100);
  }
  {
    // a gap does not touch the other kinds' runs
    ProblemWatch w;
    failEvery(w, FL, T0, 2 * HOUR);
    w.failed(WE, T0);
    w.failed(WE, T0 + 2 * HOUR);  // weather: gap, restarts
    CHECK(w.kindIsLong(FL, T0 + 2 * HOUR));
    CHECK(!w.kindIsLong(WE, T0 + 2 * HOUR));
    CHECK(w.failingForMs(FL, T0 + 2 * HOUR) == 2 * HOUR);
  }
  {
    // ok() and clear() still work after a restart, and a restart after clear() is a plain first failure
    ProblemWatch w;
    w.failed(FL, T0);
    w.failed(FL, T0 + 3 * HOUR);
    w.ok(FL);
    CHECK(w.failingForMs(FL, T0 + 3 * HOUR + 1) == 0);
    w.failed(FL, T0 + 3 * HOUR + 2);
    CHECK(w.failingForMs(FL, T0 + 3 * HOUR + 12) == 10);
    w.clear();
    w.failed(FL, T0 + 3 * HOUR + 1000);
    CHECK(w.failingForMs(FL, T0 + 3 * HOUR + 1001) == 1);
  }
}

static void scenarioKinds(uint32_t T0) {
  for (int k = 0; k < PROBLEM_KINDS; k++) {  // any one kind alone triggers it, and only that kind
    ProblemWatch w;
    failEvery(w, (ProblemKind)k, T0, HOUR);
    CHECK(w.kindIsLong((ProblemKind)k, T0 + HOUR));
    CHECK(w.longFailure(T0 + HOUR));
    for (int j = 0; j < PROBLEM_KINDS; j++)
      if (j != k) { CHECK(!w.kindIsLong((ProblemKind)j, T0 + HOUR)); CHECK(w.failingForMs((ProblemKind)j, T0 + HOUR) == 0); }
    w.ok((ProblemKind)((k + 1) % PROBLEM_KINDS));  // another kind succeeding does not end this one
    CHECK(w.longFailure(T0 + HOUR));
    w.ok((ProblemKind)k);
    CHECK(!w.longFailure(T0 + HOUR));
  }
  {
    ProblemWatch w;  // two kinds failing at once are not added up: 40 minutes each is not an hour
    failEvery(w, FL, T0, 40 * MIN);
    failEvery(w, WE, T0, 40 * MIN);
    CHECK(!w.longFailure(T0 + 40 * MIN));
  }
  {
    ProblemWatch w;  // one kind after the other is not added up either
    failEvery(w, FL, T0, 40 * MIN);
    w.ok(FL);
    failEvery(w, WE, T0 + 40 * MIN + 15000, 40 * MIN);
    CHECK(!w.longFailure(T0 + 80 * MIN));
  }
  {
    ProblemWatch w;  // flights long, weather just started: flights still counts
    failEvery(w, FL, T0, 2 * HOUR);
    failEvery(w, WE, T0 + HOUR, HOUR);
    CHECK(w.kindIsLong(FL, T0 + 2 * HOUR));
    w.ok(WE);
    CHECK(w.kindIsLong(FL, T0 + 2 * HOUR));
  }
  {
    ProblemWatch w;  // with several kinds failing, the message stays until the last long one clears
    failEvery(w, FL, T0, HOUR);
    failEvery(w, HE, T0, HOUR);
    w.ok(FL);
    CHECK(w.longFailure(T0 + HOUR));
    w.ok(HE);
    CHECK(!w.longFailure(T0 + HOUR));
  }
}

static void scenarioClear(uint32_t T0) {
  ProblemWatch w;
  failEvery(w, FL, T0, HOUR);
  failEvery(w, WE, T0, HOUR);
  failEvery(w, UP, T0, HOUR);
  CHECK(w.longFailure(T0 + HOUR));
  w.clear();  // the owner changed the settings
  CHECK(!w.longFailure(T0 + HOUR));
  CHECK(!w.kindIsLong(FL, T0 + HOUR) && !w.kindIsLong(WE, T0 + HOUR) && !w.kindIsLong(UP, T0 + HOUR));
  CHECK(w.failingForMs(FL, T0 + HOUR) == 0 && w.failingForMs(WE, T0 + HOUR) == 0 && w.failingForMs(UP, T0 + HOUR) == 0);
  uint32_t t1 = T0 + HOUR + 1000;  // the next failure gets a fresh hour
  failEvery(w, FL, t1, HOUR - 15000);
  CHECK(!w.kindIsLong(FL, t1 + HOUR - 1));
  CHECK(w.kindIsLong(FL, t1 + HOUR));
  CHECK(!w.kindIsLong(WE, t1 + HOUR));  // the other kinds stay cleared
  w.clear();
  w.clear();  // twice is fine
  CHECK(!w.longFailure(t1 + HOUR));
  ProblemWatch fresh;
  fresh.clear();  // clearing nothing is fine
  CHECK(!fresh.longFailure(T0 + HOUR));
}

static void scenarioDue(uint32_t T0, bool exactCounts) {
  // T0 is a multiple of 100 ms here (the loop samples every 100 ms)
  ProblemWatch w;
  uint32_t start = T0 + HOUR;
  failEvery(w, FL, T0, HOUR);
  int due = 0, phase = 0, samples = 0;
  for (uint64_t d = 0; d < 180000; d += 100) {  // three minutes with the failure going on, tried every 15 s
    uint32_t now = (uint32_t)(start + d);
    if (d % 15000 == 0) w.failed(FL, now);
    samples++;
    if (problemMessagePhase(now)) phase++;
    if (wanted(w, now)) due++;
    CHECK(wanted(w, now) == (problemMessagePhase(now) && w.longFailure(now)));
  }
  CHECK(samples == 1800);
  if (exactCounts) {  // (not across the wrap of millis(): the free-running minute is one window longer or shorter there)
    CHECK(phase == 150);  // 5 s in every 60 s, three times
    CHECK(due == 150);    // and all of them while the long failure lasts
  }
}

static void scenariosAt(uint32_t T0, const char *label) {
  (void)label;
  scenarioHour(T0);
  scenarioSuccessRestarts(T0);
  scenarioStale(T0);
  scenarioGap(T0);
  scenarioKinds(T0);
  scenarioClear(T0);
  bool wraps = (uint64_t)T0 + HOUR + 180000 > 0xFFFFFFFFull;
  scenarioDue(T0 - T0 % 100, !wraps);
}

static void testWatch() {
  SECTION("ProblemWatch: the one-hour rule (several start times, wrapping millis() included)");
  const uint32_t bases[] = {0u,          1u,          15000u,      3u * HOUR,    0x7FFFFFFFu - HOUR / 2, 0x80000000u,
                            0xFFFFFFFFu - 4 * HOUR, 0xFFFFFFFFu - 2 * HOUR, 0xFFFFFFFFu - HOUR - 7, 0xFFFFFFFFu - HOUR,
                            0xFFFFFFFFu - 30 * MIN, 0xFFFFFFFFu - 15000, 0xFFFFFFFFu - 1, 0xFFFFFFFFu};
  for (uint32_t b : bases) {
    int before = fails;
    scenariosAt(b, "");
    if (fails != before) printf("    (the failures above were with the start at millis() = %u)\n", (unsigned)b);
  }
}

static void testWatchWrapDirect() {
  SECTION("ProblemWatch: millis() wraps at 2^32");
  {
    // since before the wrap, last and now after it
    uint32_t since = 0xFFFFFFFFu - 30 * MIN + 1;  // 30 minutes before the wrap
    ProblemWatch w;
    failEvery(w, FL, since, HOUR);  // the last failure is 30 minutes after the wrap (as a number: about 1.8 million)
    uint32_t lastFail = (uint32_t)(since + HOUR);
    CHECK(lastFail < since);  // it really wrapped
    CHECK(w.kindIsLong(FL, lastFail));
    CHECK(w.failingForMs(FL, lastFail) == HOUR);
  }
  {
    // last just before the wrap, now just after: the failure is 1 minute old, not 49 days old
    ProblemWatch w;
    failEvery(w, WE, 0xFFFFFFFFu - HOUR - 60000 + 1, HOUR);
    uint32_t last = (uint32_t)(0xFFFFFFFFu - 60000 + 1);
    CHECK(w.kindIsLong(WE, last));
    CHECK(w.kindIsLong(WE, last + 60004));  // now = 4, after the wrap
    CHECK(w.kindIsLong(WE, (uint32_t)(last + PROBLEM_FRESH_MS - 1)));
    CHECK(!w.kindIsLong(WE, (uint32_t)(last + PROBLEM_FRESH_MS + 1)));
  }
  {
    // the failure starts at the last millisecond before the wrap
    ProblemWatch w;
    w.failed(FL, 0xFFFFFFFFu);
    CHECK(w.failingForMs(FL, 0) == 1);
    CHECK(w.failingForMs(FL, 1) == 2);
    CHECK(w.failingForMs(FL, HOUR - 1) == HOUR);
    for (uint32_t t = 0; t < HOUR; t += 15000) w.failed(FL, t);  // last failure 14999 ms before HOUR - 1
    CHECK(w.kindIsLong(FL, HOUR - 1));
    CHECK(!w.kindIsLong(FL, HOUR - 2));
  }
  {
    // a gap of 24 days between two failures: the run starts again; only differences are used
    ProblemWatch w;
    w.failed(FL, 0);
    w.failed(FL, 0x80000000u);
    CHECK(!w.kindIsLong(FL, 0x80000000u));
    CHECK(w.failingForMs(FL, 0x80000000u) == 0);
  }
  {
    // the gap rule across the wrap: exactly PROBLEM_FRESH_MS keeps the run, one more ms restarts it
    ProblemWatch keep, restart;
    uint32_t t0 = 0xFFFFFFFFu - 100;
    keep.failed(FL, t0);
    keep.failed(FL, (uint32_t)(t0 + PROBLEM_FRESH_MS));
    CHECK(keep.failingForMs(FL, (uint32_t)(t0 + PROBLEM_FRESH_MS + 7)) == PROBLEM_FRESH_MS + 7);
    restart.failed(FL, t0);
    restart.failed(FL, (uint32_t)(t0 + PROBLEM_FRESH_MS + 1));
    CHECK(restart.failingForMs(FL, (uint32_t)(t0 + PROBLEM_FRESH_MS + 1)) == 0);
  }
}

static void testWatchOutOfRange() {
  SECTION("ProblemWatch: out-of-range kinds are ignored");
  struct Rig { unsigned char pre[32]; ProblemWatch w; unsigned char post[32]; Rig() { memset(pre, 0xA5, 32); memset(post, 0xA5, 32); } };
  Rig rig;
  ProblemWatch &w = rig.w;
  ProblemWatch before = w;
  for (ProblemKind bad : {ProblemKind::Count, (ProblemKind)7, (ProblemKind)100, (ProblemKind)255}) {
    for (uint32_t t = 0; t <= HOUR + 15000; t += 15000) w.failed(bad, t);
    w.ok(bad);
    CHECK(w.failingForMs(bad, HOUR + 15000) == 0);
    CHECK(!w.kindIsLong(bad, HOUR + 15000));
  }
  CHECK(!w.longFailure(HOUR + 15000));
  CHECK(memcmp(&before, &w, sizeof w) == 0);  // nothing changed anywhere in the object
  bool guardsOk = true;
  for (unsigned char b : rig.pre) if (b != 0xA5) guardsOk = false;
  for (unsigned char b : rig.post) if (b != 0xA5) guardsOk = false;
  CHECK(guardsOk);
  // while real kinds are failing, a bad kind neither joins nor ends them
  failEvery(w, FL, 0, HOUR);
  w.failed(ProblemKind::Count, HOUR);
  w.ok(ProblemKind::Count);
  w.ok((ProblemKind)200);
  w.clear();  // clear() touches every real kind and nothing else
  CHECK(!w.longFailure(HOUR));
  failEvery(w, FL, 2 * HOUR, HOUR);
  w.failed((ProblemKind)9, 3 * HOUR);
  CHECK(w.kindIsLong(FL, 3 * HOUR));
  CHECK(!w.kindIsLong((ProblemKind)9, 3 * HOUR));
  bool guardsOk2 = true;
  for (unsigned char b : rig.pre) if (b != 0xA5) guardsOk2 = false;
  for (unsigned char b : rig.post) if (b != 0xA5) guardsOk2 = false;
  CHECK(guardsOk2);
}

static void testPhase() {
  SECTION("problemMessagePhase and the one-hour rule through problemMessageWanted");
  CHECK(problemMessagePhase(0));
  CHECK(problemMessagePhase(1));
  CHECK(problemMessagePhase(4999));
  CHECK(!problemMessagePhase(5000));
  CHECK(!problemMessagePhase(5001));
  CHECK(!problemMessagePhase(30000));
  CHECK(!problemMessagePhase(59999));
  CHECK(problemMessagePhase(60000));
  CHECK(problemMessagePhase(60000 + 4999));
  CHECK(!problemMessagePhase(60000 + 5000));
  CHECK(!problemMessagePhase(119999));
  CHECK(problemMessagePhase(120000));
  CHECK(problemMessagePhase(HOUR));
  CHECK(problemMessagePhase(HOUR + 4999));
  CHECK(!problemMessagePhase(HOUR + 5000));
  CHECK(!problemMessagePhase(HOUR - 1));
  CHECK(problemMessagePhase(4294920000u));          // 71582 whole minutes
  CHECK(problemMessagePhase(4294920000u + 4999));
  CHECK(!problemMessagePhase(4294920000u + 5000));
  CHECK(!problemMessagePhase(0xFFFFFFFFu));          // 47295 ms into its minute
  // every second of three minutes: the first 5 of each minute and no others
  for (uint32_t s = 0; s < 180; s++) {
    CHECK(problemMessagePhase(s * 1000) == (s % 60 < 5));
    CHECK(problemMessagePhase(s * 1000 + 999) == (s % 60 < 5));
  }

  // due = a long failure AND the phase
  uint32_t T0 = 600000;  // a multiple of a minute
  ProblemWatch w;
  failEvery(w, FL, T0, HOUR);
  uint32_t at = T0 + HOUR;  // the hour is up and it is the start of a minute
  CHECK(w.longFailure(at) && problemMessagePhase(at));
  CHECK(wanted(w, at));
  CHECK(wanted(w, at + 4999));
  CHECK(!wanted(w, at + 5000));  // phase over, long failure still on
  CHECK(w.longFailure(at + 5000));
  CHECK(!wanted(w, at + 30000));
  CHECK(!wanted(w, at + 59999));
  CHECK(wanted(w, at + MIN));  // the next minute, 1 minute after the last failure
  // a phase without a long failure
  ProblemWatch none;
  CHECK(!wanted(none, 0));
  CHECK(!wanted(none, at));
  ProblemWatch shortRun;
  failEvery(shortRun, FL, T0, 30 * MIN);
  CHECK(!wanted(shortRun, T0 + 30 * MIN));  // 30 minutes of failure at the start of a minute: no message
  // the problem clears inside the window: the message stops at once
  w.ok(FL);
  CHECK(!wanted(w, at));
  // the failure stops being fresh: the message stops by itself
  ProblemWatch stale;
  failEvery(stale, FL, T0, HOUR);
  uint32_t m = at + 10 * MIN;  // 10 minutes after the last failure, at the start of a minute
  CHECK(m % MIN == 0);
  CHECK(wanted(stale, m - MIN));
  CHECK(!wanted(stale, m + MIN));
  // settings saved: the message stops at once
  ProblemWatch s2;
  failEvery(s2, WE, T0, HOUR);
  CHECK(wanted(s2, at));
  s2.clear();
  CHECK(!wanted(s2, at));
}

static void testMessageWanted() {
  SECTION("problemMessageWanted: when the message replaces the picture");
  const uint32_t IN = 600000;          // a minute boundary: 0 ms into the minute, inside the 5 s phase
  const uint32_t IN2 = 600000 + 4999;  // the last ms of the phase
  const uint32_t OUT = 600000 + 5000;  // the first ms after it
  CHECK(problemMessagePhase(IN) && problemMessagePhase(IN2) && !problemMessagePhase(OUT));
  // the one case where it shows: live, a long failure, inside the phase, a static picture, no night blanking
  CHECK(problemMessageWanted(true, true, IN, true, false));
  CHECK(problemMessageWanted(true, true, IN2, true, false));
  // a plane card or a headline is never replaced (staticPicture false), whatever else is true
  CHECK(!problemMessageWanted(true, true, IN, false, false));
  CHECK(!problemMessageWanted(true, true, IN2, false, false));
  // night blanking stays dark; the 30 s NightAwake window (night false) may show it
  CHECK(!problemMessageWanted(true, true, IN, true, true));
  CHECK(problemMessageWanted(true, true, IN, true, false));
  // outside the 5 s phase
  for (uint32_t t : {OUT, IN + 5001, IN + 30000, IN + 59999}) CHECK(!problemMessageWanted(true, true, t, true, false));
  // not live data (the example screens), no long failure
  CHECK(!problemMessageWanted(false, true, IN, true, false));
  CHECK(!problemMessageWanted(true, false, IN, true, false));
  CHECK(!problemMessageWanted(false, false, IN, false, true));
  // all 32 combinations against the plain sentence of the rule, at a time inside the phase and one outside it
  for (uint32_t t : {IN, OUT}) {
    for (int m = 0; m < 16; m++) {
      bool live = m & 1, lf = m & 2, st = m & 4, night = m & 8;
      bool want = live && lf && st && !night && t == IN;
      CHECK(problemMessageWanted(live, lf, t, st, night) == want);
    }
  }
  // with a real watch: the message follows the failure and stops when it clears or goes stale
  ProblemWatch w;
  failEvery(w, FL, IN - HOUR, HOUR);  // failing for exactly an hour, the last failure at IN
  CHECK(problemMessageWanted(true, w.longFailure(IN), IN, true, false));
  CHECK(!problemMessageWanted(true, w.longFailure(IN), IN, false, false));  // a plane is on the panel
  w.ok(FL);
  CHECK(!problemMessageWanted(true, w.longFailure(IN), IN, true, false));   // the problem cleared
  ProblemWatch stale;
  failEvery(stale, FL, IN - HOUR, HOUR);
  uint32_t later = IN + 11 * MIN;  // a minute boundary, 11 minutes after the last failure
  CHECK(problemMessagePhase(later));
  CHECK(!problemMessageWanted(true, stale.longFailure(later), later, true, false));
  ProblemWatch young;
  failEvery(young, FL, IN - HOUR + 1, HOUR - 1);  // one ms short of an hour... the last failure at IN
  CHECK(!problemMessageWanted(true, young.longFailure(IN), IN, true, false));
  // millis() wraps: a long failure that started before the wrap and is still failing after it, asked in the phase after
  // the wrap (the first 5 s of the new count) and in the phase before it
  {
    ProblemWatch a;
    uint32_t after = 1000;  // 1 s after the wrap
    failEvery(a, WE, (uint32_t)(after - HOUR), HOUR);  // since is just before 2^32 minus an hour, last failure at `after`
    CHECK((uint32_t)(after - HOUR) > after);            // it really wrapped
    CHECK(a.longFailure(after));
    CHECK(problemMessageWanted(true, a.longFailure(after), after, true, false));
    CHECK(problemMessageWanted(true, a.longFailure(4999), 4999, true, false));
    CHECK(!problemMessageWanted(true, a.longFailure(5000), 5000, true, false));
    CHECK(!problemMessageWanted(true, a.longFailure(after), after, false, false));
    CHECK(!problemMessageWanted(true, a.longFailure(after), after, true, true));
    ProblemWatch b;
    uint32_t before = 4294920000u + 100;  // 71582 whole minutes plus 100 ms: inside the phase, just before the wrap
    failEvery(b, WE, before - HOUR, HOUR);
    CHECK(problemMessagePhase(before));
    CHECK(problemMessageWanted(true, b.longFailure(before), before, true, false));
    CHECK(!problemMessageWanted(true, b.longFailure(before), before, false, false));
    CHECK(!problemMessageWanted(true, b.longFailure(before), before, true, true));
    // carrying on through the wrap with a failure every 15 s: still long (the rule uses differences), the phase is the
    // free-running one
    for (uint64_t d = 0; d <= 120000; d += 1000) {
      uint32_t now = (uint32_t)(before + d);
      if (d % 15000 == 0) b.failed(WE, now);
      CHECK(b.longFailure(now));
      CHECK(problemMessageWanted(true, true, now, true, false) == problemMessagePhase(now));
    }
  }
}

int main() {
  testConstants();
  testProblemHas();
  testKindNames();
  testWordsBranches();
  testWordsConnectPrefixes();
  testWordsOrdering();
  testWordsGreedy();
  testWordsCaseAndStatusForms();
  testRawMacros();
  testWordsSentences();
  testWordsDeviceAuth();
  testWordsDetail();
  testWordsUnknown();
  testWordsIdempotent();
  testWordsBounds();
  testLogBasics();
  testLogRepeat();
  testLogIdentity();
  testLogCapacity();
  testLogFlood();
  testLogCounter();
  testLogClockNotSet();
  testLogTruncation();
  testLogLongRepeat();
  testLogRecordRaw();
  testWatch();
  testWatchWrapDirect();
  testWatchOutOfRange();
  testPhase();
  testMessageWanted();
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
