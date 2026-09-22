#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string>
#include <cppunit/extensions/HelperMacros.h>

#include <arc/message/PayloadStream.h>

#include "PayloadHTTP.h"

// Chunked transfer encoding puts a length on the wire before every chunk.
// These tests cover what happens when that length is not one the reader can
// honour, which is what a truncated or rewritten stream produces.
class PayloadHTTPChunkedTest: public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(PayloadHTTPChunkedTest);
  CPPUNIT_TEST(TestWellFormedChunks);
  CPPUNIT_TEST(TestNegativeChunkSize);
  CPPUNIT_TEST(TestGarbageChunkSize);
  CPPUNIT_TEST_SUITE_END();
public:
  void setUp() {
    CPPUNIT_ASSERT_EQUAL(0, pipe(fds));
  }
  void tearDown() {
    close(fds[0]);
    close(fds[1]);
  }
  void TestWellFormedChunks();
  void TestNegativeChunkSize();
  void TestGarbageChunkSize();
private:
  int fds[2];
  // Feed a complete chunked response and hand back the parsed payload. The
  // write end is closed so the reader sees a clean end of stream rather than
  // blocking once the supplied bytes run out.
  void feed(const std::string& data) {
    CPPUNIT_ASSERT_EQUAL((ssize_t)data.size(),
                         write(fds[1], data.data(), data.size()));
    close(fds[1]);
    fds[1] = dup(fds[0]); // keep tearDown's second close valid
  }
  static std::string response(const std::string& body) {
    return std::string("HTTP/1.1 200 OK\r\n")
         + "Transfer-Encoding: chunked\r\n"
         + "\r\n" + body;
  }
};

void PayloadHTTPChunkedTest::TestWellFormedChunks() {
  feed(response("4\r\ndata\r\n0\r\n\r\n"));
  Arc::PayloadStream stream(fds[0]);
  ArcMCCHTTP::PayloadHTTPIn payload(stream);
  CPPUNIT_ASSERT(payload);
  char buf[32];
  int size = sizeof(buf);
  CPPUNIT_ASSERT(payload.Get(buf, size));
  CPPUNIT_ASSERT_EQUAL(4, size);
  CPPUNIT_ASSERT_EQUAL(std::string("data"), std::string(buf, 4));
}

void PayloadHTTPChunkedTest::TestNegativeChunkSize() {
  // strtoll() honours a leading sign even in base 16, so "-1" parses as a
  // valid number and leaves a negative chunk size. That reached memcpy() as
  // a very large size_t. Reading must fail instead.
  feed(response("-1\r\ndata\r\n0\r\n\r\n"));
  Arc::PayloadStream stream(fds[0]);
  ArcMCCHTTP::PayloadHTTPIn payload(stream);
  char buf[32];
  int size = sizeof(buf);
  CPPUNIT_ASSERT(!payload.Get(buf, size));
}

void PayloadHTTPChunkedTest::TestGarbageChunkSize() {
  feed(response("zz\r\ndata\r\n0\r\n\r\n"));
  Arc::PayloadStream stream(fds[0]);
  ArcMCCHTTP::PayloadHTTPIn payload(stream);
  char buf[32];
  int size = sizeof(buf);
  CPPUNIT_ASSERT(!payload.Get(buf, size));
}

CPPUNIT_TEST_SUITE_REGISTRATION(PayloadHTTPChunkedTest);
