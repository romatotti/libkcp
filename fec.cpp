//
// Created by 理 傅 on 2017/1/2.
//

#include <err.h>
#include <sys/time.h>
#include "fec.h"
#include "ikcp.h"
#include "sess.h"

/* encode 16 bits unsigned int (lsb) */
static inline char *encode16u(char *p, unsigned short w)
{
#if IWORDS_BIG_ENDIAN
    *(unsigned char*)(p + 0) = (w & 255);
	*(unsigned char*)(p + 1) = (w >> 8);
#else
    *(unsigned short*)(p) = w;
#endif
    p += 2;
    return p;
}

/* decode 16 bits unsigned int (lsb) */
static inline char *decode16u(char *p, unsigned short *w)
{
#if IWORDS_BIG_ENDIAN
    *w = *(const unsigned char*)(p + 1);
	*w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
    *w = *(const unsigned short*)p;
#endif
    p += 2;
    return p;
}

/* encode 32 bits unsigned int (lsb) */
static inline char *encode32u(char *p, IUINT32 l)
{
#if IWORDS_BIG_ENDIAN
    *(unsigned char*)(p + 0) = (unsigned char)((l >>  0) & 0xff);
	*(unsigned char*)(p + 1) = (unsigned char)((l >>  8) & 0xff);
	*(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
	*(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
    *(IUINT32*)p = l;
#endif
    p += 4;
    return p;
}

/* decode 32 bits unsigned int (lsb) */
static inline char *decode32u(char *p, IUINT32 *l)
{
#if IWORDS_BIG_ENDIAN
    *l = *(const unsigned char*)(p + 3);
	*l = *(const unsigned char*)(p + 2) + (*l << 8);
	*l = *(const unsigned char*)(p + 1) + (*l << 8);
	*l = *(const unsigned char*)(p + 0) + (*l << 8);
#else
    *l = *(const IUINT32*)p;
#endif
    p += 4;
    return p;
}

FEC::FEC(ReedSolomon enc) :enc(enc) {}

FEC
FEC::newFEC(int rxlimit, int dataShards, int parityShards)  {
    if (dataShards <= 0 || parityShards <= 0) {
        throw std::invalid_argument("invalid arguments");
    }

    if (rxlimit < dataShards+parityShards) {
        throw std::invalid_argument("invalid arguments");
    }

    FEC fec(ReedSolomon::New(dataShards, parityShards));
    fec.rxlimit = rxlimit;
    fec.dataShards = dataShards;
    fec.parityShards = parityShards;
    fec.totalShards = dataShards + parityShards;
    fec.paws = (0xffffffff/uint32_t(fec.totalShards) - 1) * uint32_t(fec.totalShards);

    return fec;
}

fecPacket
FEC::decode(char* data, size_t sz) {
    fecPacket pkt;
    data = decode32u(data, &pkt.seqid);
    data = decode16u(data, &pkt.flag);
    struct timeval time;
    gettimeofday(&time, NULL);
    pkt.ts = uint32_t(time.tv_sec * 1000 + time.tv_usec/1000);
    pkt.data = std::make_shared<std::vector<byte>>(data, data+sz);
    return pkt;
}

void
FEC::markData(char *data) {
    data = encode32u(data,this->next);
    data = encode16u(data,typeData);
    this->next++;
}

void
FEC::markFEC(char *data) {
    data = encode32u(data,this->next);
    data = encode16u(data,typeFEC);
    this->next++;
    if (this->next >= this->paws) { // paws would only occurs in markFEC
        this->next = 0;
    }
}

int
FEC::input(fecPacket &pkt, std::vector<byte *> &recovered) {
    uint32_t now = currentMs();
    if (now-lastCheck >= fecExpire) {
        for (auto it = rx.begin();it !=rx.end();) {
            if (now - it->ts > fecExpire) {
                it = rx.erase(it);
            } else {
                it++;
            }
        }
        lastCheck = now;
    }

    // insertion
    int n = this->rx.size() -1;
    int insertIdx = 0;
    for (int i=n;i>=0;i--) {
        if (pkt.seqid == rx[i].seqid) {
            return 0;
        } else if (pkt.seqid > rx[i].seqid) {
            insertIdx = i + 1;
            break;
        }
    }
    // insert into ordered rx queue
    rx.insert(rx.begin()+insertIdx, pkt);

    // shard range for current packet
    int shardBegin = pkt.seqid - pkt.seqid%totalShards;
    int shardEnd = shardBegin + totalShards - 1;

    // max search range in ordered queue for current shard
    int searchBegin = insertIdx - pkt.seqid%totalShards;
    if (searchBegin < 0) {
        searchBegin = 0;
    }

    int searchEnd = searchBegin + totalShards - 1;
    if (searchEnd >= rx.size()) {
        searchEnd = rx.size()-1;
    }

    if (searchEnd > searchBegin && searchEnd-searchBegin+1 >= dataShards) {
        int numshard = 0;
        int numDataShard = 0;
        int first = -1;
        int maxlen = 0;

        std::vector<int> indices(totalShards, -1);

        for (int i = searchBegin; i <= searchEnd; i++) {
            auto seqid = rx[i].seqid;
            if (seqid > shardEnd) {
                break;
            } else if (seqid >= shardBegin) {
                indices[seqid%totalShards] = i;
                numshard++;
                if (rx[i].flag == typeData) {
                    numDataShard++;
                }
                if (numshard == 1) {
                    first = i;
                }
                if (rx[i].data->size() > maxlen) {
                    maxlen = rx[i].data->size();
                }
            }
        }

        if (numDataShard == dataShards) { // no lost
            rx.erase(rx.begin()+first, rx.begin() + first+numshard);
        } else if (numshard >= dataShards) { // recoverable
            std::vector<row> shardVec(totalShards);
            for (int k=0;k<totalShards;k++){
                if (indices[k] != -1) {
                    shardVec[k] = rx[indices[k]].data;
                }
            }

            if (int ret = enc.Reconstruct(shardVec) && ret== 0 ){
            }
            rx.erase(rx.begin()+first, rx.begin() + first+numshard);
        }
    }

    // keep rxlimit
    if (rx.size() > rxlimit) {
        rx.erase(rx.begin());
    }

    return 0;
}

std::vector<row>
FEC::calcECC(std::vector<row> &data) {
    std::vector<row> shards(totalShards);

    unsigned long maxlen = 0;
    for (int i=0;i<dataShards;i++) {
        shards[i] = data[i];
        if (maxlen < data[i]->size()) {
            maxlen = data[i]->size();
        }
    }

    for (int i=0;i<shards.size();i++) {
        shards[i]->resize(maxlen);
    }

    enc.Encode(shards);
    return std::vector<row>(shards.begin()+dataShards, shards.end());
}


