#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string>
#include <cppunit/extensions/HelperMacros.h>

#include <arc/message/PayloadStream.h>

#include "../BIOGSIMCC.h"

// GSI frames every token as a 4 byte big endian length followed by that many
// bytes. The length is taken straight off the wire, so these tests cover what
// happens when it is not a length this code can honour - which is what a
// truncated or rewritten stream produces.
class BIOGSIMCCTest: public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(BIOGSIMCCTest);
  CPPUNIT_TEST(TestWholeToken);
  CPPUNIT_TEST(TestSplitHeader);
  CPPUNIT_TEST(TestLengthWithTopBitSet);
  CPPUNIT_TEST(TestOversizedLength);
  CPPUNIT_TEST_SUITE_END();
public:
  void setUp() {
    CPPUNIT_ASSERT_EQUAL(0, pipe(fds));
    stream = new Arc::PayloadStream(fds[0]);
    bio = ArcMCCTLS::BIO_new_GSIMCC(stream);
    CPPUNIT_ASSERT(bio != NULL);
  }
  void tearDown() {
    if(bio) BIO_free(bio);
    bio = NULL;
    // The BIO owns neither the stream nor the read end once freed, so close
    // whatever is left behind.
    close(fds[1]);
  }
  void TestWholeToken();
  void TestSplitHeader();
  void TestLengthWithTopBitSet();
  void TestOversizedLength();
private:
  int fds[2];
  Arc::PayloadStream* stream;
  BIO* bio;
  // Feed raw bytes to the read end of the BIO.
  void feed(const std::string& data) {
    CPPUNIT_ASSERT_EQUAL((ssize_t)data.size(),
                         write(fds[1], data.data(), data.size()));
  }
  static std::string header(unsigned int length) {
    std::string h(4, '\0');
    h[0] = (char)((length>>24)&0xff);
    h[1] = (char)((length>>16)&0xff);
    h[2] = (char)((length>>8)&0xff);
    h[3] = (char)((length>>0)&0xff);
    return h;
  }
};

void BIOGSIMCCTest::TestWholeToken() {
  feed(header(5) + "hello");
  char buf[16];
  CPPUNIT_ASSERT_EQUAL(5, BIO_read(bio, buf, sizeof(buf)));
  CPPUNIT_ASSERT_EQUAL(std::string("hello"), std::string(buf, 5));
}

void BIOGSIMCCTest::TestSplitHeader() {
  // A length prefix arriving in pieces must still be assembled correctly;
  // the reader keeps a running count rather than assuming one read suffices.
  std::string const framed = header(4) + "data";
  feed(framed.substr(0, 2));
  char buf[16];
  // Nothing can be delivered from half a length prefix, and the caller must
  // be told to come back rather than being handed a byte count.
  CPPUNIT_ASSERT_EQUAL(-1, BIO_read(bio, buf, sizeof(buf)));
  CPPUNIT_ASSERT(BIO_should_retry(bio));
  feed(framed.substr(2));
  CPPUNIT_ASSERT_EQUAL(4, BIO_read(bio, buf, sizeof(buf)));
  CPPUNIT_ASSERT_EQUAL(std::string("data"), std::string(buf, 4));
}

void BIOGSIMCCTest::TestLengthWithTopBitSet() {
  // 0x80000000 read back as a signed int is negative. It then skipped the
  // clamp against the output buffer and reached read() as a huge size_t.
  // Body bytes follow, so a build that failed to reject the length would
  // deliver them and report a positive count rather than -1.
  feed(header(0x80000000u) + "data");
  char buf[16];
  CPPUNIT_ASSERT_EQUAL(-1, BIO_read(bio, buf, sizeof(buf)));
}

void BIOGSIMCCTest::TestOversizedLength() {
  // A plausible-looking but far too large length is refused outright rather
  // than being consumed in buffer-sized pieces.
  feed(header(0xffffffffu) + "data");
  char buf[16];
  CPPUNIT_ASSERT_EQUAL(-1, BIO_read(bio, buf, sizeof(buf)));
}

CPPUNIT_TEST_SUITE_REGISTRATION(BIOGSIMCCTest);
