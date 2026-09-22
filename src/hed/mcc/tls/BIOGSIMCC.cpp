#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cstdlib>
#include <cstring>

#include <openssl/ssl.h>

#include <arc/message/PayloadRaw.h>
#include <arc/message/PayloadStream.h>
#include <arc/message/MCC.h>
#include <arc/message/Message.h>

#include "BIOGSIMCC.h"

namespace ArcMCCTLS {

using namespace Arc;

// Upper bound on a GSI token length read off the wire. Tokens carry handshake
// data and wrapped SSL records, which stay far below this, so a larger value
// means the length prefix has been corrupted rather than that a bigger token
// is genuinely in flight.
static const unsigned int max_token_size = 16*1024*1024;


class BIOGSIMCC {
  private:
    PayloadStreamInterface* stream_;
    MCCInterface* next_;
    unsigned int header_;
    unsigned int token_;
    MCC_Status result_;
    BIO_METHOD* biom_;
    BIO* bio_; // not owned by this object
  public:
    BIOGSIMCC(MCCInterface* next):result_(STATUS_OK) {
      next_=NULL; stream_=NULL;
      header_=4; token_=0;
      bio_ = NULL;
      if(MakeMethod()) {
        bio_ = BIO_new(biom_);
        if(bio_) {
          next_=next;
          BIO_set_data(bio_,this);
        }
      }
    };
    BIOGSIMCC(PayloadStreamInterface* stream):result_(STATUS_OK) {
      next_=NULL; stream_=NULL;
      header_=4; token_=0;
      bio_ = NULL;
      if(MakeMethod()) {
        bio_ = BIO_new(biom_);
        if(bio_) {
          stream_=stream;
          BIO_set_data(bio_,this);
        }
      }
    };
    ~BIOGSIMCC(void) {
      if(stream_ && next_) delete stream_;
      if(biom_) BIO_meth_free(biom_);
    };
    BIO* GetBIO() const { return bio_; };
    PayloadStreamInterface* Stream() const { return stream_; };
    void Stream(PayloadStreamInterface* stream) { stream_=stream; /*free ??*/ };
    MCCInterface* Next(void) const { return next_; };
    void MCC(MCCInterface* next) { next_=next; };
    // Kept unsigned end to end: these hold a length taken off the wire, and
    // returning it as int made a token of 2GiB or more read back negative.
    unsigned int Header(void) const { return header_; };
    void Header(unsigned int v) { header_=v; };
    unsigned int Token(void) const { return token_; };
    void Token(unsigned int v) { token_=v; };
    const MCC_Status& Result(void) { return result_; };
  private:
    static int  mcc_write(BIO *h, const char *buf, int num);
    static int  mcc_read(BIO *h, char *buf, int size);
    static int  mcc_puts(BIO *h, const char *str);
    static long mcc_ctrl(BIO *h, int cmd, long arg1, void *arg2);
    static int  mcc_new(BIO *h);
    static int  mcc_free(BIO *data);
    bool MakeMethod(void) {
      biom_ = BIO_meth_new(BIO_TYPE_FD,"Message Chain Component");
      if(biom_) {
        (void)BIO_meth_set_write(biom_,&BIOGSIMCC::mcc_write);
        (void)BIO_meth_set_read(biom_,&BIOGSIMCC::mcc_read);
        (void)BIO_meth_set_puts(biom_,&BIOGSIMCC::mcc_puts);
        (void)BIO_meth_set_ctrl(biom_,&BIOGSIMCC::mcc_ctrl);
        (void)BIO_meth_set_create(biom_,&BIOGSIMCC::mcc_new);
        (void)BIO_meth_set_destroy(biom_,&BIOGSIMCC::mcc_free);
      }
      return (biom_ != NULL);
    };
};


BIO *BIO_new_GSIMCC(MCCInterface* mcc) {
  BIOGSIMCC* biomcc = new BIOGSIMCC(mcc);
  BIO* bio = biomcc->GetBIO();
  if(!bio) delete biomcc;
  return bio;
}   
    
BIO* BIO_new_GSIMCC(PayloadStreamInterface* stream) { 
  BIOGSIMCC* biomcc = new BIOGSIMCC(stream);
  BIO* bio = biomcc->GetBIO();
  if(!bio) delete biomcc;
  return bio;
}   

int BIOGSIMCC::mcc_new(BIO *bi) {
  BIO_set_data(bi,NULL);
  BIO_set_init(bi,1);
  return(1);
}

int BIOGSIMCC::mcc_free(BIO *b) {
  if(b == NULL) return(0);
  BIOGSIMCC* biomcc = (BIOGSIMCC*)(BIO_get_data(b));
  BIO_set_data(b,NULL);
  if(biomcc) delete biomcc;
  return(1);
}
  
int BIOGSIMCC::mcc_read(BIO *b, char *out,int outl) {
  int ret=0;
  if (out == NULL) return(ret);
  if(b == NULL) return(ret);
  // Nothing can be delivered into an empty buffer, and a negative size would
  // otherwise be carried into the length arithmetic below.
  if(outl <= 0) return(ret);
  BIOGSIMCC* biomcc = (BIOGSIMCC*)(BIO_get_data(b));
  if(biomcc == NULL) return(ret);
  PayloadStreamInterface* stream = biomcc->Stream();
  if(stream == NULL) return ret;
  //clear_sys_error();
  BIO_clear_retry_flags(b);
  if(biomcc->Header()) {
    unsigned char header[4];
    // How much of the 4 byte length prefix is still outstanding. Clamped so
    // that a corrupted counter can never index outside the local array.
    unsigned int pending = biomcc->Header();
    if(pending > sizeof(header)) pending = sizeof(header);
    unsigned int const offset = sizeof(header) - pending;
    int l = (int)pending;
    if(!stream->Get((char*)(header+offset),l)) return -1;
    // Get() reports how much it actually delivered, so only those bytes are
    // folded into the length being assembled.
    for(int n = (int)offset;n<(int)offset+l;++n) {
      biomcc->Token(biomcc->Token() | (((unsigned int)header[n]) << ((3-n)*8)));
    };
    biomcc->Header(pending-(unsigned int)l);
    if(biomcc->Header() != 0) {
      // Only part of the length prefix has arrived, so no payload can be
      // delivered on this call. Ask to be called again rather than falling
      // through: the old code left outl at the caller's buffer size and
      // reported that as the byte count, handing back whatever the buffer
      // already held.
      BIO_set_retry_read(b);
      return -1;
    };
  };
  unsigned int remaining = biomcc->Token();
  if(remaining > max_token_size) {
    // Not a size this code can ever legitimately be asked to read, so the
    // stream is corrupted or hostile. Refuse it here rather than passing the
    // value on: previously a prefix with the top bit set arrived as a
    // negative int, skipped the clamp against outl, and reached read() as a
    // huge size_t.
    return -1;
  };
  int delivered = 0;
  if(remaining) {
    int l = (remaining > (unsigned int)outl)?outl:(int)remaining;
    if(!stream->Get(out,l)) return -1;
    biomcc->Token(remaining - (unsigned int)l);
    delivered = l;
  };
  if(biomcc->Token() == 0) biomcc->Header(4);
  return delivered;
}

int BIOGSIMCC::mcc_write(BIO *b, const char *in, int inl) {
  int ret = 0;
  //clear_sys_error();
  if(in == NULL) return(ret);
  if(b == NULL) return(ret);
  if(BIO_get_data(b) == NULL) return(ret);
  BIOGSIMCC* biomcc = (BIOGSIMCC*)(BIO_get_data(b));
  if(biomcc == NULL) return(ret);
  unsigned char header[4];
  header[0]=(inl>>24)&0xff;
  header[1]=(inl>>16)&0xff;
  header[2]=(inl>>8)&0xff;
  header[3]=(inl>>0)&0xff;
  PayloadStreamInterface* stream = biomcc->Stream();
  if(stream != NULL) {
    // If available just use stream directly
    // TODO: check if stream has changed ???
    bool r;
    r = stream->Put((const char*)header,4);
    if(r) r = stream->Put(in,inl);
    BIO_clear_retry_flags(b);
    if(r) { ret=inl; } else { ret=-1; };
    return ret;
  };

  MCCInterface* next = biomcc->Next();
  if(next == NULL) return(ret);
  PayloadRaw nextpayload;
  nextpayload.Insert((const char*)header,0,4);
  nextpayload.Insert(in,4,inl); // Not efficient !!!!
  Message nextinmsg;
  nextinmsg.Payload(&nextpayload);  
  Message nextoutmsg;

  MCC_Status mccret = next->process(nextinmsg,nextoutmsg);
  BIO_clear_retry_flags(b);
  if(mccret) { 
    if(nextoutmsg.Payload()) {
      PayloadStreamInterface* retpayload = NULL;
      try {
        retpayload=dynamic_cast<PayloadStreamInterface*>(nextoutmsg.Payload());
      } catch(std::exception& e) { };
      if(retpayload) {
        biomcc->Stream(retpayload);
      } else {
        delete nextoutmsg.Payload();
      };
    };
    ret=inl;
  } else {
    if(nextoutmsg.Payload()) delete nextoutmsg.Payload();
    ret=-1;
  };
  return(ret);
}


long BIOGSIMCC::mcc_ctrl(BIO*, int cmd, long, void*) {
  long ret=0;

  switch (cmd) {
    case BIO_CTRL_RESET:
    case BIO_CTRL_SET_CLOSE:
    case BIO_CTRL_SET:
    case BIO_CTRL_EOF:
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DUP:
      ret=1;
      break;
  };
  return(ret);
}

int BIOGSIMCC::mcc_puts(BIO *bp, const char *str) {
  int n,ret;

  n=strlen(str);
  ret=mcc_write(bp,str,n);
  return(ret);
}

bool BIO_GSIMCC_failure(BIO* bio, MCC_Status& s) {
  if(!bio) return false;
  BIOGSIMCC* b = (BIOGSIMCC*)(BIO_get_data(bio));
  if(!b || b->Result()) return false;
  s = b->Result();
  return true;
}

} // namespace Arc
